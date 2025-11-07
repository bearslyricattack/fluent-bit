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
#include <msgpack.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "filter_pod.h"

/* 从文件读取内容到 buffer */
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

    ret = stat(path, &st);
    if (ret == -1) {
        flb_errno();
        fclose(fp);
        return -1;
    }

    buf = flb_calloc(1, st.st_size + 1);
    if (!buf) {
        flb_errno();
        fclose(fp);
        return -1;
    }

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

/* 初始化插件 */
static int cb_pod_init(struct flb_filter_instance *f_ins,
                      struct flb_config *config,
                      void *data)
{
    int ret;
    struct flb_filter_pod *ctx;
    char *token = NULL;
    size_t token_size = 0;
    const char *tmp;

    /* 分配插件上下文 */
    ctx = flb_calloc(1, sizeof(struct flb_filter_pod));
    if (!ctx) {
        flb_errno();
        return -1;
    }

    ctx->ins = f_ins;
    ctx->config = config;

    /* 读取配置参数 */

    /* API Server 配置 */
    tmp = flb_filter_get_property("api_host", f_ins);
    ctx->api_host = tmp ? flb_strdup(tmp) : flb_strdup(FLB_POD_DEFAULT_API_HOST);

    tmp = flb_filter_get_property("api_port", f_ins);
    ctx->api_port = tmp ? atoi(tmp) : FLB_POD_DEFAULT_API_PORT;

    tmp = flb_filter_get_property("use_tls", f_ins);
    ctx->use_tls = tmp ? flb_utils_bool(tmp) : FLB_TRUE;

    /* Token 和证书路径配置 */
    tmp = flb_filter_get_property("token_path", f_ins);
    ctx->token_path = tmp ? flb_strdup(tmp) : flb_strdup(FLB_POD_DEFAULT_TOKEN_PATH);

    tmp = flb_filter_get_property("ca_path", f_ins);
    ctx->ca_path = tmp ? flb_strdup(tmp) : flb_strdup(FLB_POD_DEFAULT_CA_PATH);

    /* Pod ID 字段名配置 */
    tmp = flb_filter_get_property("pod_id_field", f_ins);
    ctx->pod_id_field = tmp ? flb_strdup(tmp) : flb_strdup("pod_id");

    /* 元数据丰富选项 */
    tmp = flb_filter_get_property("add_labels", f_ins);
    ctx->add_labels = tmp ? flb_utils_bool(tmp) : FLB_TRUE;

    tmp = flb_filter_get_property("add_annotations", f_ins);
    ctx->add_annotations = tmp ? flb_utils_bool(tmp) : FLB_FALSE;

    tmp = flb_filter_get_property("add_namespace", f_ins);
    ctx->add_namespace = tmp ? flb_utils_bool(tmp) : FLB_TRUE;

    tmp = flb_filter_get_property("add_pod_name", f_ins);
    ctx->add_pod_name = tmp ? flb_utils_bool(tmp) : FLB_TRUE;

    tmp = flb_filter_get_property("add_node_name", f_ins);
    ctx->add_node_name = tmp ? flb_utils_bool(tmp) : FLB_TRUE;

    /* 缓存 TTL 配置 */
    tmp = flb_filter_get_property("cache_ttl", f_ins);
    ctx->cache_ttl = tmp ? atoi(tmp) : FLB_POD_CACHE_TTL;

    /* 读取 Kubernetes token */
    ret = file_to_buffer(ctx->token_path, &token, &token_size);
    if (ret == -1) {
        flb_plg_warn(f_ins, "cannot read token from %s, API requests may fail",
                    ctx->token_path);
    }
    else {
        ctx->token = token;
        ctx->token_len = token_size;
        flb_plg_info(f_ins, "loaded Kubernetes token from %s", ctx->token_path);
    }

    /* 创建 upstream 连接 */
    if (ctx->use_tls) {
        ctx->upstream = flb_upstream_create(config,
                                          ctx->api_host,
                                          ctx->api_port,
                                          FLB_IO_TLS,
                                          NULL);
    }
    else {
        ctx->upstream = flb_upstream_create(config,
                                          ctx->api_host,
                                          ctx->api_port,
                                          FLB_IO_TCP,
                                          NULL);
    }

    if (!ctx->upstream) {
        flb_plg_error(f_ins, "failed to create upstream connection to %s:%d",
                     ctx->api_host, ctx->api_port);
        if (ctx->token) flb_free(ctx->token);
        flb_free(ctx->api_host);
        flb_free(ctx->token_path);
        flb_free(ctx->ca_path);
        flb_free(ctx->pod_id_field);
        flb_free(ctx);
        return -1;
    }

    /* 创建元数据缓存 */
    ctx->pod_cache = flb_hash_table_create(FLB_HASH_TABLE_EVICT_NONE, 256, -1);
    if (!ctx->pod_cache) {
        flb_plg_error(f_ins, "failed to create metadata cache");
        flb_upstream_destroy(ctx->upstream);
        if (ctx->token) flb_free(ctx->token);
        flb_free(ctx->api_host);
        flb_free(ctx->token_path);
        flb_free(ctx->ca_path);
        flb_free(ctx->pod_id_field);
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

/* 将 labels 或 annotations 添加到日志记录
 * 简化版本:暂时不实现,后续可扩展
 */
static void add_hash_table_to_record(struct flb_log_event_encoder *enc,
                                    const char *key,
                                    struct flb_hash_table *hash_table)
{
    /* TODO: 实现 labels 和 annotations 的添加 */
    /* 由于哈希表结构复杂,这里暂时跳过 */
    return;
}

/* 过滤回调函数 */
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
            for (i = 0; i < obj->via.map.size; i++) {
                kv = &obj->via.map.ptr[i];

                if (kv->key.type == MSGPACK_OBJECT_STR &&
                    kv->val.type == MSGPACK_OBJECT_STR) {

                    if (strncmp(kv->key.via.str.ptr, ctx->pod_id_field,
                               kv->key.via.str.size) == 0) {
                        pod_id = kv->val.via.str.ptr;
                        pod_id_len = kv->val.via.str.size;
                        break;
                    }
                }
            }
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

        /* 如果找到了 pod_id,获取并添加元数据 */
        if (pod_id && pod_id_len > 0) {
            char pod_id_str[256];
            snprintf(pod_id_str, sizeof(pod_id_str), "%.*s",
                    (int)pod_id_len, pod_id);

            flb_plg_debug(f_ins, "found pod_id: %s", pod_id_str);

            /* 获取 pod 元数据 */
            ret = flb_pod_get_metadata(ctx, pod_id_str, &meta);
            if (ret == 0 && meta) {
                modified = FLB_TRUE;

                /* 添加 pod 名称 */
                if (ctx->add_pod_name && meta->pod_name) {
                    flb_log_event_encoder_append_body_cstring(
                        &log_encoder, "kubernetes_pod_name");
                    flb_log_event_encoder_append_body_cstring(
                        &log_encoder, meta->pod_name);
                }

                /* 添加 namespace */
                if (ctx->add_namespace && meta->namespace) {
                    flb_log_event_encoder_append_body_cstring(
                        &log_encoder, "kubernetes_namespace");
                    flb_log_event_encoder_append_body_cstring(
                        &log_encoder, meta->namespace);
                }

                /* 添加 node 名称 */
                if (ctx->add_node_name && meta->node_name) {
                    flb_log_event_encoder_append_body_cstring(
                        &log_encoder, "kubernetes_node_name");
                    flb_log_event_encoder_append_body_cstring(
                        &log_encoder, meta->node_name);
                }

                /* 添加 labels */
                if (ctx->add_labels && meta->labels) {
                    add_hash_table_to_record(&log_encoder,
                                           "kubernetes_labels",
                                           meta->labels);
                }

                /* 添加 annotations */
                if (ctx->add_annotations && meta->annotations) {
                    add_hash_table_to_record(&log_encoder,
                                           "kubernetes_annotations",
                                           meta->annotations);
                }

                flb_plg_debug(f_ins, "enriched log with pod metadata");
            }
            else {
                flb_plg_warn(f_ins, "failed to get metadata for pod_id: %s",
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

/* 退出插件 */
static int cb_pod_exit(void *data, struct flb_config *config)
{
    struct flb_filter_pod *ctx = data;

    if (!ctx) {
        return 0;
    }

    /* 清理缓存(需要遍历并释放 pod_meta 结构) */
    if (ctx->pod_cache) {
        /* TODO: 遍历哈希表并释放所有 flb_pod_meta 结构 */
        flb_hash_table_destroy(ctx->pod_cache);
    }

    /* 清理 upstream */
    if (ctx->upstream) {
        flb_upstream_destroy(ctx->upstream);
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
     "Path to Kubernetes CA certificate"
    },
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
