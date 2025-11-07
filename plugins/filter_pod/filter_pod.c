/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2024 The Fluent Bit Authors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_filter.h>
#include <fluent-bit/flb_filter_plugin.h>
#include <fluent-bit/flb_config.h>
#include <fluent-bit/flb_mem.h>
#include <fluent-bit/flb_str.h>
#include <fluent-bit/flb_time.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_log_event_decoder.h>
#include <fluent-bit/flb_log_event_encoder.h>
#include <fluent-bit/flb_upstream.h>
#include <fluent-bit/flb_io.h>
#include <fluent-bit/flb_hash_table.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/tls/flb_tls.h>
#include <msgpack.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "filter_pod.h"

/**
 * 从文件读取内容到 buffer
 * 用于读取 Kubernetes ServiceAccount token 等文件
 *
 * @param path 文件路径
 * @param out_buf 输出参数，返回分配的 buffer（调用者需要释放）
 * @param out_size 输出参数，返回读取的字节数
 * @return 成功返回 0，失败返回 -1
 */
static int file_to_buffer(const char *path, char **out_buf, size_t *out_size)
{
    int ret;
    char *buf;
    ssize_t bytes;
    FILE *fp;
    struct stat st;

    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    /* 获取文件大小 */
    ret = stat(path, &st);
    if (ret == -1) {
        flb_errno();
        fclose(fp);
        return -1;
    }

    /* 分配 buffer，额外 +1 用于 null 终止符 */
    buf = flb_calloc(1, st.st_size + 1);
    if (!buf) {
        flb_errno();
        fclose(fp);
        return -1;
    }

    /* 读取文件内容 */
    bytes = fread(buf, st.st_size, 1, fp);
    if (bytes < 1) {
        flb_free(buf);
        fclose(fp);
        return -1;
    }

    fclose(fp);

    *out_buf = buf;
    *out_size = st.st_size;

    return 0;
}

/**
 * 插件初始化回调函数
 * 负责：
 * 1. 读取和验证配置参数
 * 2. 加载 Kubernetes ServiceAccount token
 * 3. 创建到 API Server 的 upstream 连接（支持 TLS）
 * 4. 初始化元数据缓存
 *
 * @param f_ins 过滤器实例
 * @param config Fluent Bit 配置
 * @param data 用户数据（未使用）
 * @return 成功返回 0，失败返回 -1
 */
static int cb_pod_init(struct flb_filter_instance *f_ins,
                      struct flb_config *config,
                      void *data)
{
    int ret;
    int io_type;
    struct flb_filter_pod *ctx;
    char *token = NULL;
    size_t token_size = 0;

    /* 分配插件上下文 */
    ctx = flb_calloc(1, sizeof(struct flb_filter_pod));
    if (!ctx) {
        flb_errno();
        return -1;
    }

    ctx->ins = f_ins;
    ctx->config = config;
    ctx->tls = NULL;

    /* 使用 config_map 设置属性 */
    ret = flb_filter_config_map_set(f_ins, (void *)ctx);
    if (ret == -1) {
        flb_free(ctx);
        return -1;
    }

    /* 如果没有设置 api_host，使用默认值 */
    if (!ctx->api_host) {
        ctx->api_host = flb_strdup(FLB_POD_DEFAULT_API_HOST);
    }

    /* 如果没有设置 token_path，使用默认值 */
    if (!ctx->token_path) {
        ctx->token_path = flb_strdup(FLB_POD_DEFAULT_TOKEN_PATH);
    }

    /* 如果没有设置 pod_id_field，使用默认值 */
    if (!ctx->pod_id_field) {
        ctx->pod_id_field = flb_strdup("pod_id");
    }

    /* 读取 Kubernetes token */
    ret = file_to_buffer(ctx->token_path, &token, &token_size);
    if (ret == -1) {
        flb_plg_warn(f_ins, "cannot read token from %s, API requests may fail",
                    ctx->token_path);
    }
    else {
        /* 清除 token 末尾的换行符 */
        while (token_size > 0 &&
               (token[token_size - 1] == '\n' || token[token_size - 1] == '\r')) {
            token[--token_size] = '\0';
        }
        ctx->token = token;
        ctx->token_len = token_size;
        flb_plg_info(f_ins, "loaded Kubernetes token from %s (%zu bytes)",
                    ctx->token_path, token_size);
    }

    /* 创建 TLS 配置（如果启用） */
    if (ctx->use_tls == FLB_TRUE) {
        /* 如果没有设置 CA 文件，使用默认值 */
        if (!ctx->tls_ca_file && !ctx->tls_ca_path) {
            ctx->tls_ca_file = flb_strdup(FLB_POD_DEFAULT_CA_PATH);
        }

        /* 创建 TLS 上下文 */
        ctx->tls = flb_tls_create(FLB_TLS_CLIENT_MODE,
                                  ctx->tls_verify,
                                  ctx->tls_debug,
                                  ctx->tls_vhost,
                                  ctx->tls_ca_path,
                                  ctx->tls_ca_file,
                                  NULL, NULL, NULL);
        if (!ctx->tls) {
            flb_plg_error(f_ins, "failed to create TLS context");
            if (ctx->token) flb_free(ctx->token);
            if (ctx->api_host) flb_free(ctx->api_host);
            if (ctx->token_path) flb_free(ctx->token_path);
            if (ctx->pod_id_field) flb_free(ctx->pod_id_field);
            if (ctx->tls_ca_file) flb_free(ctx->tls_ca_file);
            flb_free(ctx);
            return -1;
        }

        /* 设置主机名验证 */
        if (ctx->tls_verify_hostname == FLB_TRUE) {
            ret = flb_tls_set_verify_hostname(ctx->tls, ctx->tls_verify_hostname);
            if (ret == -1) {
                flb_plg_warn(f_ins, "failed to set TLS hostname verification");
            }
        }

        io_type = FLB_IO_TLS;
        flb_plg_info(f_ins, "TLS enabled with CA file: %s",
                    ctx->tls_ca_file ? ctx->tls_ca_file : ctx->tls_ca_path);
    }
    else {
        io_type = FLB_IO_TCP;
    }

    /* 创建 upstream 连接 */
    ctx->upstream = flb_upstream_create(config,
                                       ctx->api_host,
                                       ctx->api_port,
                                       io_type,
                                       ctx->tls);

    if (!ctx->upstream) {
        flb_plg_error(f_ins, "failed to create upstream connection to %s:%d (TLS: %s)",
                     ctx->api_host, ctx->api_port, ctx->use_tls ? "enabled" : "disabled");
        if (ctx->tls) {
            flb_tls_destroy(ctx->tls);
        }
        if (ctx->token) flb_free(ctx->token);
        if (ctx->api_host) flb_free(ctx->api_host);
        if (ctx->token_path) flb_free(ctx->token_path);
        if (ctx->pod_id_field) flb_free(ctx->pod_id_field);
        if (ctx->tls_ca_file) flb_free(ctx->tls_ca_file);
        if (ctx->ca_path) flb_free(ctx->ca_path);
        flb_free(ctx);
        return -1;
    }

    /* 创建元数据缓存 */
    ctx->pod_cache = flb_hash_table_create(FLB_HASH_TABLE_EVICT_NONE, 256, -1);
    if (!ctx->pod_cache) {
        flb_plg_error(f_ins, "failed to create metadata cache");
        flb_upstream_destroy(ctx->upstream);
        if (ctx->tls) {
            flb_tls_destroy(ctx->tls);
        }
        if (ctx->token) flb_free(ctx->token);
        if (ctx->api_host) flb_free(ctx->api_host);
        if (ctx->token_path) flb_free(ctx->token_path);
        if (ctx->pod_id_field) flb_free(ctx->pod_id_field);
        if (ctx->tls_ca_file) flb_free(ctx->tls_ca_file);
        if (ctx->ca_path) flb_free(ctx->ca_path);
        flb_free(ctx);
        return -1;
    }

    /* 设置插件上下文 */
    flb_filter_set_context(f_ins, ctx);

    flb_plg_info(f_ins, "filter_pod initialized successfully");
    flb_plg_info(f_ins, "  API Server: %s:%d (TLS: %s)",
                ctx->api_host, ctx->api_port, ctx->use_tls ? "enabled" : "disabled");
    flb_plg_info(f_ins, "  Pod ID field: %s", ctx->pod_id_field);
    flb_plg_info(f_ins, "  Cache TTL: %d seconds", ctx->cache_ttl);
    flb_plg_info(f_ins, "  Metadata enrichment: labels=%s, annotations=%s, namespace=%s, pod_name=%s, node_name=%s",
                ctx->add_labels ? "yes" : "no",
                ctx->add_annotations ? "yes" : "no",
                ctx->add_namespace ? "yes" : "no",
                ctx->add_pod_name ? "yes" : "no",
                ctx->add_node_name ? "yes" : "no");

    return 0;
}

/**
 * 将 labels 或 annotations 哈希表添加到日志记录
 * 将哈希表中的键值对作为一个 map 添加到日志记录中
 *
 * @param enc 日志事件编码器
 * @param key 在日志记录中使用的字段名（如 "kubernetes_labels"）
 * @param hash_table 包含键值对的哈希表
 * @return 成功返回 0，失败返回 -1
 */
static int add_hash_table_to_record(struct flb_log_event_encoder *enc,
                                    const char *key,
                                    struct flb_hash_table *hash_table)
{
    int ret;
    struct mk_list *head;
    struct flb_hash_table_entry *entry;
    int entry_count = 0;

    if (!enc || !key || !hash_table) {
        return -1;
    }

    /* 首先统计有多少个条目 */
    mk_list_foreach(head, &hash_table->entries) {
        entry_count++;
    }

    if (entry_count == 0) {
        /* 如果没有条目，不添加空 map */
        return 0;
    }

    /* 添加字段名 */
    ret = flb_log_event_encoder_append_body_cstring(enc, (char *)key);
    if (ret != FLB_EVENT_ENCODER_SUCCESS) {
        return -1;
    }

    /* 开始一个 map - 使用 raw_msgpack API */
    msgpack_sbuffer sbuf;
    msgpack_packer packer;

    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&packer, &sbuf, msgpack_sbuffer_write);

    msgpack_pack_map(&packer, entry_count);

    /* 遍历哈希表，添加所有键值对 */
    mk_list_foreach(head, &hash_table->entries) {
        entry = mk_list_entry(head, struct flb_hash_table_entry, _head);

        /* 添加键 */
        msgpack_pack_str(&packer, entry->key_len);
        msgpack_pack_str_body(&packer, entry->key, entry->key_len);

        /* 添加值 - 检查 val 是否为字符串 */
        if (entry->val_size > 0) {
            msgpack_pack_str(&packer, entry->val_size);
            msgpack_pack_str_body(&packer, entry->val, entry->val_size);
        }
        else {
            /* 如果没有指定大小，假设是 null 终止的字符串 */
            size_t val_len = entry->val ? strlen((char *)entry->val) : 0;
            msgpack_pack_str(&packer, val_len);
            if (val_len > 0) {
                msgpack_pack_str_body(&packer, entry->val, val_len);
            }
        }
    }

    /* 添加打包的 map 到 body */
    ret = flb_log_event_encoder_append_raw_msgpack(enc, FLB_LOG_EVENT_BODY,
                                                   sbuf.data, sbuf.size);

    msgpack_sbuffer_destroy(&sbuf);

    if (ret != FLB_EVENT_ENCODER_SUCCESS) {
        return -1;
    }

    return 0;
}

/**
 * 过滤回调函数 - 核心处理逻辑
 * 工作流程：
 * 1. 解码输入的日志事件流
 * 2. 遍历每条日志记录，查找 pod_id 字段
 * 3. 根据 pod_id（UID）从 Kubernetes API 获取 Pod 元数据
 * 4. 将 Pod 元数据（名称、namespace、labels 等）添加到日志记录
 * 5. 编码并返回增强后的日志流
 *
 * @param data 输入日志数据（msgpack 格式）
 * @param bytes 输入数据大小
 * @param tag 日志标签
 * @param tag_len 标签长度
 * @param out_buf 输出缓冲区
 * @param out_bytes 输出数据大小
 * @param f_ins 过滤器实例
 * @param i_ins 输入实例
 * @param filter_context 过滤器上下文
 * @param config Fluent Bit 配置
 * @return FLB_FILTER_MODIFIED 如果修改了数据，FLB_FILTER_NOTOUCH 如果未修改
 */
static int cb_pod_filter(const void *data, size_t bytes,
                        const char *tag, int tag_len,
                        void **out_buf, size_t *out_bytes,
                        struct flb_filter_instance *f_ins,
                        struct flb_input_instance *i_ins,
                        void *filter_context,
                        struct flb_config *config)
{
    struct flb_filter_pod *ctx = filter_context;
    struct flb_log_event_decoder log_decoder;
    struct flb_log_event_encoder log_encoder;
    struct flb_log_event log_event;
    int ret;
    msgpack_object *obj;
    msgpack_object_kv *kv;
    const char *pod_id = NULL;
    size_t pod_id_len = 0;
    struct flb_pod_meta *meta = NULL;
    int modified = FLB_FALSE;
    int i;

    /* 初始化 decoder 和 encoder */
    ret = flb_log_event_decoder_init(&log_decoder, (char *)data, bytes);
    if (ret != FLB_EVENT_DECODER_SUCCESS) {
        flb_plg_error(f_ins, "failed to initialize log event decoder");
        return FLB_FILTER_NOTOUCH;
    }

    ret = flb_log_event_encoder_init(&log_encoder,
                                    FLB_LOG_EVENT_FORMAT_DEFAULT);
    if (ret != FLB_EVENT_ENCODER_SUCCESS) {
        flb_plg_error(f_ins, "failed to initialize log event encoder");
        flb_log_event_decoder_destroy(&log_decoder);
        return FLB_FILTER_NOTOUCH;
    }

    /* 处理每条日志记录 */
    while ((ret = flb_log_event_decoder_next(&log_decoder, &log_event))
           == FLB_EVENT_DECODER_SUCCESS) {

        /* 查找 pod_id 字段 */
        pod_id = NULL;
        pod_id_len = 0;

        obj = log_event.body;
        if (obj->type == MSGPACK_OBJECT_MAP) {
            /* 遍历日志记录中的所有字段，查找 pod_id */
            for (i = 0; i < obj->via.map.size; i++) {
                kv = &obj->via.map.ptr[i];

                if (kv->key.type == MSGPACK_OBJECT_STR &&
                    kv->val.type == MSGPACK_OBJECT_STR) {

                    if (strncmp(kv->key.via.str.ptr, ctx->pod_id_field,
                               kv->key.via.str.size) == 0 &&
                        strlen(ctx->pod_id_field) == kv->key.via.str.size) {
                        pod_id = kv->val.via.str.ptr;
                        pod_id_len = kv->val.via.str.size;
                        flb_plg_debug(f_ins, "Found pod_id field: %.*s",
                                    (int)pod_id_len, pod_id);
                        break;
                    }
                }
            }
        }
        else {
            flb_plg_debug(f_ins, "Log event body is not a map, skipping");
        }

        /* 开始编码新记录 */
        ret = flb_log_event_encoder_begin_record(&log_encoder);
        if (ret != FLB_EVENT_ENCODER_SUCCESS) {
            flb_plg_error(f_ins, "failed to begin log event record");
            continue;
        }

        /* 设置时间戳 */
        ret = flb_log_event_encoder_set_timestamp(&log_encoder,
                                                 &log_event.timestamp);
        if (ret != FLB_EVENT_ENCODER_SUCCESS) {
            flb_plg_error(f_ins, "failed to set timestamp");
            continue;
        }

        /* 复制原始日志字段 */
        ret = flb_log_event_encoder_set_body_from_msgpack_object(
            &log_encoder, log_event.body);
        if (ret != FLB_EVENT_ENCODER_SUCCESS) {
            flb_plg_error(f_ins, "failed to set body");
            continue;
        }

        /* 如果找到了 pod_id，获取并添加元数据 */
        if (pod_id && pod_id_len > 0) {
            char pod_id_str[256];
            int meta_added = 0;

            /* 确保 pod_id 不超过 buffer 大小 */
            if (pod_id_len >= sizeof(pod_id_str)) {
                flb_plg_warn(f_ins, "pod_id too long (%zu bytes), truncating",
                           pod_id_len);
                pod_id_len = sizeof(pod_id_str) - 1;
            }

            snprintf(pod_id_str, sizeof(pod_id_str), "%.*s",
                    (int)pod_id_len, pod_id);

            /* 获取 pod 元数据（可能从缓存或 API Server） */
            ret = flb_pod_get_metadata(ctx, pod_id_str, &meta);
            if (ret == 0 && meta) {
                modified = FLB_TRUE;

                /* 添加 pod 名称 */
                if (ctx->add_pod_name && meta->pod_name) {
                    ret = flb_log_event_encoder_append_body_cstring(
                        &log_encoder, "kubernetes_pod_name");
                    if (ret == FLB_EVENT_ENCODER_SUCCESS) {
                        ret = flb_log_event_encoder_append_body_cstring(
                            &log_encoder, meta->pod_name);
                        if (ret == FLB_EVENT_ENCODER_SUCCESS) {
                            meta_added++;
                        }
                    }
                }

                /* 添加 namespace */
                if (ctx->add_namespace && meta->namespace) {
                    ret = flb_log_event_encoder_append_body_cstring(
                        &log_encoder, "kubernetes_namespace");
                    if (ret == FLB_EVENT_ENCODER_SUCCESS) {
                        ret = flb_log_event_encoder_append_body_cstring(
                            &log_encoder, meta->namespace);
                        if (ret == FLB_EVENT_ENCODER_SUCCESS) {
                            meta_added++;
                        }
                    }
                }

                /* 添加 node 名称 */
                if (ctx->add_node_name && meta->node_name) {
                    ret = flb_log_event_encoder_append_body_cstring(
                        &log_encoder, "kubernetes_node_name");
                    if (ret == FLB_EVENT_ENCODER_SUCCESS) {
                        ret = flb_log_event_encoder_append_body_cstring(
                            &log_encoder, meta->node_name);
                        if (ret == FLB_EVENT_ENCODER_SUCCESS) {
                            meta_added++;
                        }
                    }
                }

                /* 添加 labels */
                if (ctx->add_labels && meta->labels) {
                    ret = add_hash_table_to_record(&log_encoder,
                                                  "kubernetes_labels",
                                                  meta->labels);
                    if (ret == 0) {
                        meta_added++;
                    }
                }

                /* 添加 annotations */
                if (ctx->add_annotations && meta->annotations) {
                    ret = add_hash_table_to_record(&log_encoder,
                                                  "kubernetes_annotations",
                                                  meta->annotations);
                    if (ret == 0) {
                        meta_added++;
                    }
                }

                if (meta_added > 0) {
                    flb_plg_debug(f_ins, "Enriched log with %d metadata fields for pod %s",
                                meta_added, pod_id_str);
                }
            }
            else {
                flb_plg_debug(f_ins, "Failed to get metadata for pod_id: %s (will retry next time)",
                           pod_id_str);
            }
        }

        /* 完成记录编码 */
        ret = flb_log_event_encoder_commit_record(&log_encoder);
        if (ret != FLB_EVENT_ENCODER_SUCCESS) {
            flb_plg_error(f_ins, "failed to commit log event record");
        }
    }

    /* 清理 decoder */
    flb_log_event_decoder_destroy(&log_decoder);

    /* 如果有修改,输出新数据 */
    if (modified == FLB_TRUE) {
        *out_buf = log_encoder.output_buffer;
        *out_bytes = log_encoder.output_length;

        ret = FLB_FILTER_MODIFIED;

        /* 重置 encoder 但不销毁 output_buffer */
        flb_log_event_encoder_reset(&log_encoder);
    }
    else {
        ret = FLB_FILTER_NOTOUCH;
        flb_log_event_encoder_destroy(&log_encoder);
    }

    return ret;
}

/**
 * 插件退出回调函数
 * 负责清理所有分配的资源：
 * 1. 释放缓存中的所有 pod_meta 结构
 * 2. 销毁 upstream 连接
 * 3. 销毁 TLS 上下文
 * 4. 释放所有配置字符串和 token
 *
 * @param data 插件上下文
 * @param config Fluent Bit 配置
 * @return 成功返回 0
 */
static int cb_pod_exit(void *data, struct flb_config *config)
{
    struct flb_filter_pod *ctx = data;
    struct mk_list *head;
    struct flb_hash_table_entry *entry;
    struct flb_pod_meta *meta;

    if (!ctx) {
        return 0;
    }

    /* 清理缓存 - 遍历并释放所有 flb_pod_meta 结构 */
    if (ctx->pod_cache) {
        struct mk_list *tmp;
        mk_list_foreach_safe(head, tmp, &ctx->pod_cache->entries) {
            entry = mk_list_entry(head, struct flb_hash_table_entry, _head);

            /* 释放 pod_meta 结构 */
            if (entry->val) {
                meta = (struct flb_pod_meta *)entry->val;
                flb_pod_meta_destroy(meta);
            }
        }
        flb_hash_table_destroy(ctx->pod_cache);
    }

    /* 清理 upstream */
    if (ctx->upstream) {
        flb_upstream_destroy(ctx->upstream);
    }

    /* 清理 TLS */
    if (ctx->tls) {
        flb_tls_destroy(ctx->tls);
    }

    /* 清理配置字符串 */
    if (ctx->api_host) {
        flb_free(ctx->api_host);
    }
    if (ctx->token_path) {
        flb_free(ctx->token_path);
    }
    if (ctx->ca_path) {
        flb_free(ctx->ca_path);
    }
    if (ctx->tls_ca_file) {
        flb_free(ctx->tls_ca_file);
    }
    if (ctx->tls_ca_path) {
        flb_free(ctx->tls_ca_path);
    }
    if (ctx->tls_vhost) {
        flb_free(ctx->tls_vhost);
    }
    if (ctx->pod_id_field) {
        flb_free(ctx->pod_id_field);
    }
    if (ctx->token) {
        flb_free(ctx->token);
    }

    flb_free(ctx);

    return 0;
}

/* 插件配置参数 */
static struct flb_config_map config_map[] = {
    {
     FLB_CONFIG_MAP_STR, "api_host", FLB_POD_DEFAULT_API_HOST,
     0, FLB_TRUE, offsetof(struct flb_filter_pod, api_host),
     "Kubernetes API server host"
    },
    {
     FLB_CONFIG_MAP_INT, "api_port", "443",
     0, FLB_TRUE, offsetof(struct flb_filter_pod, api_port),
     "Kubernetes API server port"
    },
    {
     FLB_CONFIG_MAP_BOOL, "use_tls", "true",
     0, FLB_TRUE, offsetof(struct flb_filter_pod, use_tls),
     "Use TLS for API server connection"
    },
    {
     FLB_CONFIG_MAP_STR, "token_path", FLB_POD_DEFAULT_TOKEN_PATH,
     0, FLB_TRUE, offsetof(struct flb_filter_pod, token_path),
     "Path to Kubernetes service account token"
    },
    {
     FLB_CONFIG_MAP_STR, "ca_path", FLB_POD_DEFAULT_CA_PATH,
     0, FLB_TRUE, offsetof(struct flb_filter_pod, ca_path),
     "Path to Kubernetes CA certificate (deprecated, use tls.ca_file)"
    },

    /* TLS options */
    {
     FLB_CONFIG_MAP_BOOL, "tls.verify", "true",
     0, FLB_TRUE, offsetof(struct flb_filter_pod, tls_verify),
     "Enable TLS certificate verification"
    },
    {
     FLB_CONFIG_MAP_BOOL, "tls.verify_hostname", "false",
     0, FLB_TRUE, offsetof(struct flb_filter_pod, tls_verify_hostname),
     "Enable TLS hostname verification"
    },
    {
     FLB_CONFIG_MAP_INT, "tls.debug", "0",
     0, FLB_TRUE, offsetof(struct flb_filter_pod, tls_debug),
     "TLS debug level"
    },
    {
     FLB_CONFIG_MAP_STR, "tls.vhost", NULL,
     0, FLB_TRUE, offsetof(struct flb_filter_pod, tls_vhost),
     "TLS virtual hostname for SNI"
    },
    {
     FLB_CONFIG_MAP_STR, "tls.ca_file", NULL,
     0, FLB_TRUE, offsetof(struct flb_filter_pod, tls_ca_file),
     "TLS CA certificate file"
    },
    {
     FLB_CONFIG_MAP_STR, "tls.ca_path", NULL,
     0, FLB_TRUE, offsetof(struct flb_filter_pod, tls_ca_path),
     "TLS CA certificate directory"
    },

    /* Pod metadata options */
    {
     FLB_CONFIG_MAP_STR, "pod_id_field", "pod_id",
     0, FLB_TRUE, offsetof(struct flb_filter_pod, pod_id_field),
     "Field name containing pod ID in log records"
    },
    {
     FLB_CONFIG_MAP_BOOL, "add_labels", "true",
     0, FLB_TRUE, offsetof(struct flb_filter_pod, add_labels),
     "Add pod labels to log records"
    },
    {
     FLB_CONFIG_MAP_BOOL, "add_annotations", "false",
     0, FLB_TRUE, offsetof(struct flb_filter_pod, add_annotations),
     "Add pod annotations to log records"
    },
    {
     FLB_CONFIG_MAP_BOOL, "add_namespace", "true",
     0, FLB_TRUE, offsetof(struct flb_filter_pod, add_namespace),
     "Add pod namespace to log records"
    },
    {
     FLB_CONFIG_MAP_BOOL, "add_pod_name", "true",
     0, FLB_TRUE, offsetof(struct flb_filter_pod, add_pod_name),
     "Add pod name to log records"
    },
    {
     FLB_CONFIG_MAP_BOOL, "add_node_name", "true",
     0, FLB_TRUE, offsetof(struct flb_filter_pod, add_node_name),
     "Add node name to log records"
    },
    {
     FLB_CONFIG_MAP_INT, "cache_ttl", "3600",
     0, FLB_TRUE, offsetof(struct flb_filter_pod, cache_ttl),
     "Metadata cache TTL in seconds"
    },
    {0}
};

/* 插件注册 */
struct flb_filter_plugin filter_pod_plugin = {
    .name         = "pod",
    .description  = "Enrich logs with Kubernetes Pod metadata based on Pod ID",
    .cb_init      = cb_pod_init,
    .cb_filter    = cb_pod_filter,
    .cb_exit      = cb_pod_exit,
    .config_map   = config_map,
    .flags        = 0
};
