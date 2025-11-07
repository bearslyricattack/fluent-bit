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
#include <fluent-bit/flb_mem.h>
#include <fluent-bit/flb_str.h>
#include <fluent-bit/flb_http_client.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_hash_table.h>
#include <fluent-bit/flb_log.h>
#include <fluent-bit/flb_filter.h>
#include <fluent-bit/flb_filter_plugin.h>
#include <msgpack.h>
#include <time.h>

#include "filter_pod.h"

/**
 * 创建 pod 元数据结构
 *
 * @return 新创建的 pod 元数据结构指针，失败返回 NULL
 */
struct flb_pod_meta *flb_pod_meta_create(void)
{
    struct flb_pod_meta *meta;

    meta = flb_calloc(1, sizeof(struct flb_pod_meta));
    if (!meta) {
        flb_errno();
        return NULL;
    }

    /* 初始化哈希表存储 labels */
    meta->labels = flb_hash_table_create(FLB_HASH_TABLE_EVICT_NONE, 32, -1);
    if (!meta->labels) {
        flb_free(meta);
        return NULL;
    }

    /* 初始化哈希表存储 annotations */
    meta->annotations = flb_hash_table_create(FLB_HASH_TABLE_EVICT_NONE, 32, -1);
    if (!meta->annotations) {
        flb_hash_table_destroy(meta->labels);
        flb_free(meta);
        return NULL;
    }

    meta->cached_at = time(NULL);
    return meta;
}

/**
 * 销毁 pod 元数据结构，释放所有分配的内存
 *
 * @param meta 要销毁的 pod 元数据结构指针
 */
void flb_pod_meta_destroy(struct flb_pod_meta *meta)
{
    if (!meta) {
        return;
    }

    if (meta->pod_id) {
        flb_free(meta->pod_id);
    }
    if (meta->pod_name) {
        flb_free(meta->pod_name);
    }
    if (meta->namespace) {
        flb_free(meta->namespace);
    }
    if (meta->node_name) {
        flb_free(meta->node_name);
    }
    if (meta->labels) {
        flb_hash_table_destroy(meta->labels);
    }
    if (meta->annotations) {
        flb_hash_table_destroy(meta->annotations);
    }

    flb_free(meta);
}

/**
 * 从 msgpack 对象中提取字符串字段
 *
 * @param obj msgpack 对象（通常是一个 map）
 * @param key 要查找的字段名
 * @return 提取的字符串副本，失败返回 NULL
 */
static char *extract_json_string(msgpack_object *obj, const char *key)
{
    int i;
    msgpack_object_kv *kv;
    msgpack_object *val;

    if (obj->type != MSGPACK_OBJECT_MAP) {
        return NULL;
    }

    for (i = 0; i < obj->via.map.size; i++) {
        kv = &obj->via.map.ptr[i];

        if (kv->key.type == MSGPACK_OBJECT_STR &&
            strncmp(kv->key.via.str.ptr, key, kv->key.via.str.size) == 0 &&
            strlen(key) == kv->key.via.str.size) {

            val = &kv->val;
            if (val->type == MSGPACK_OBJECT_STR) {
                return flb_strndup(val->via.str.ptr, val->via.str.size);
            }
        }
    }

    return NULL;
}

/**
 * 从 msgpack 对象中提取 map 字段（用于 labels 和 annotations）
 *
 * @param obj msgpack 对象
 * @param key 要查找的字段名
 * @param hash_table 存储提取结果的哈希表
 * @return 成功返回 0，失败返回 -1
 */
static int extract_json_map_to_hash(msgpack_object *obj, const char *key,
                                    struct flb_hash_table *hash_table)
{
    int i, j;
    msgpack_object_kv *kv;
    msgpack_object *val;
    msgpack_object_kv *label_kv;
    char *k_str;
    char *v_str;

    if (obj->type != MSGPACK_OBJECT_MAP) {
        return -1;
    }

    for (i = 0; i < obj->via.map.size; i++) {
        kv = &obj->via.map.ptr[i];

        if (kv->key.type == MSGPACK_OBJECT_STR &&
            strncmp(kv->key.via.str.ptr, key, kv->key.via.str.size) == 0 &&
            strlen(key) == kv->key.via.str.size) {

            val = &kv->val;
            if (val->type == MSGPACK_OBJECT_MAP) {
                /* 遍历 labels 或 annotations map */
                for (j = 0; j < val->via.map.size; j++) {
                    label_kv = &val->via.map.ptr[j];

                    if (label_kv->key.type == MSGPACK_OBJECT_STR &&
                        label_kv->val.type == MSGPACK_OBJECT_STR) {

                        k_str = flb_strndup(label_kv->key.via.str.ptr,
                                          label_kv->key.via.str.size);
                        v_str = flb_strndup(label_kv->val.via.str.ptr,
                                          label_kv->val.via.str.size);

                        if (k_str && v_str) {
                            flb_hash_table_add(hash_table, k_str, strlen(k_str),
                                             v_str, strlen(v_str));
                        }

                        if (k_str) flb_free(k_str);
                        if (v_str) flb_free(v_str);
                    }
                }
                return 0;
            }
        }
    }

    return -1;
}

/**
 * 从 msgpack 对象中查找指定 key 的子对象
 *
 * @param obj msgpack 对象（map 类型）
 * @param key 要查找的 key
 * @return 找到的子对象指针，未找到返回 NULL
 */
static msgpack_object *find_map_value(msgpack_object *obj, const char *key)
{
    int i;
    msgpack_object_kv *kv;

    if (obj->type != MSGPACK_OBJECT_MAP) {
        return NULL;
    }

    for (i = 0; i < obj->via.map.size; i++) {
        kv = &obj->via.map.ptr[i];

        if (kv->key.type == MSGPACK_OBJECT_STR &&
            strncmp(kv->key.via.str.ptr, key, kv->key.via.str.size) == 0 &&
            strlen(key) == kv->key.via.str.size) {
            return &kv->val;
        }
    }

    return NULL;
}

/**
 * 解析单个 Pod 对象的元数据
 *
 * @param pod_obj Pod 对象的 msgpack 表示
 * @param meta 存储解析结果的元数据结构
 * @param ins 插件实例（用于日志）
 * @return 成功返回 0，失败返回 -1
 */
static int parse_single_pod(msgpack_object *pod_obj,
                            struct flb_pod_meta *meta,
                            struct flb_filter_instance *ins)
{
    msgpack_object *metadata;
    msgpack_object *spec;
    msgpack_object *uid;

    if (pod_obj->type != MSGPACK_OBJECT_MAP) {
        flb_plg_error(ins, "Pod object is not a map");
        return -1;
    }

    /* 获取 metadata 对象 */
    metadata = find_map_value(pod_obj, "metadata");
    if (!metadata || metadata->type != MSGPACK_OBJECT_MAP) {
        flb_plg_error(ins, "No valid metadata found in Pod object");
        return -1;
    }

    /* 提取 UID */
    uid = find_map_value(metadata, "uid");
    if (uid && uid->type == MSGPACK_OBJECT_STR) {
        if (meta->pod_id) {
            flb_free(meta->pod_id);
        }
        meta->pod_id = flb_strndup(uid->via.str.ptr, uid->via.str.size);
    }

    /* 提取 pod name */
    meta->pod_name = extract_json_string(metadata, "name");

    /* 提取 namespace */
    meta->namespace = extract_json_string(metadata, "namespace");

    /* 提取 labels */
    extract_json_map_to_hash(metadata, "labels", meta->labels);

    /* 提取 annotations */
    extract_json_map_to_hash(metadata, "annotations", meta->annotations);

    /* 获取 spec 对象 */
    spec = find_map_value(pod_obj, "spec");
    if (spec && spec->type == MSGPACK_OBJECT_MAP) {
        /* 提取 nodeName */
        meta->node_name = extract_json_string(spec, "nodeName");
    }

    flb_plg_debug(ins, "Parsed Pod metadata: uid=%s, name=%s, namespace=%s, node=%s",
                 meta->pod_id ? meta->pod_id : "null",
                 meta->pod_name ? meta->pod_name : "null",
                 meta->namespace ? meta->namespace : "null",
                 meta->node_name ? meta->node_name : "null");

    return 0;
}

/**
 * 解析 Kubernetes API 返回的 PodList 格式响应
 * 支持两种格式：
 * 1. PodList 格式（使用 fieldSelector 查询时）
 * 2. 单个 Pod 格式（直接通过名称查询时）
 *
 * @param json_data JSON 响应数据
 * @param json_len JSON 数据长度
 * @param meta 存储解析结果的元数据结构
 * @param ins 插件实例（用于日志）
 * @return 成功返回 0，失败返回 -1
 */
static int parse_pod_response(const char *json_data, size_t json_len,
                              struct flb_pod_meta *meta,
                              struct flb_filter_instance *ins)
{
    int ret;
    int root_type;
    char *buf = NULL;
    size_t buf_size;
    msgpack_unpacked result;
    msgpack_object root;
    msgpack_object *kind;
    msgpack_object *items;
    msgpack_object *first_pod;

    /* 将 JSON 转换为 msgpack */
    ret = flb_pack_json(json_data, json_len, &buf, &buf_size, &root_type, NULL);
    if (ret != 0) {
        flb_plg_error(ins, "Failed to parse JSON response");
        return -1;
    }

    /* 解包 msgpack 数据 */
    msgpack_unpacked_init(&result);
    ret = msgpack_unpack_next(&result, buf, buf_size, NULL);
    if (ret != MSGPACK_UNPACK_SUCCESS) {
        flb_plg_error(ins, "Failed to unpack msgpack data");
        flb_free(buf);
        return -1;
    }

    root = result.data;
    if (root.type != MSGPACK_OBJECT_MAP) {
        flb_plg_error(ins, "Invalid response format: root is not a map");
        msgpack_unpacked_destroy(&result);
        flb_free(buf);
        return -1;
    }

    /* 检查响应类型 */
    kind = find_map_value(&root, "kind");

    if (kind && kind->type == MSGPACK_OBJECT_STR) {
        /* 检查是否为 PodList 格式 */
        if (strncmp(kind->via.str.ptr, "PodList", 7) == 0) {
            flb_plg_debug(ins, "Parsing PodList response");

            /* 获取 items 数组 */
            items = find_map_value(&root, "items");
            if (!items || items->type != MSGPACK_OBJECT_ARRAY) {
                flb_plg_error(ins, "No items array found in PodList");
                msgpack_unpacked_destroy(&result);
                flb_free(buf);
                return -1;
            }

            /* 检查是否有 Pod */
            if (items->via.array.size == 0) {
                flb_plg_warn(ins, "No pods found in PodList response");
                msgpack_unpacked_destroy(&result);
                flb_free(buf);
                return -1;
            }

            /* 获取第一个 Pod（通过 UID 查询应该只返回一个） */
            first_pod = &items->via.array.ptr[0];
            ret = parse_single_pod(first_pod, meta, ins);

            if (items->via.array.size > 1) {
                flb_plg_warn(ins, "Multiple pods found for UID query, using first one");
            }
        }
        else if (strncmp(kind->via.str.ptr, "Pod", 3) == 0) {
            /* 单个 Pod 格式 */
            flb_plg_debug(ins, "Parsing single Pod response");
            ret = parse_single_pod(&root, meta, ins);
        }
        else {
            flb_plg_error(ins, "Unknown response kind: %.*s",
                         (int)kind->via.str.size, kind->via.str.ptr);
            ret = -1;
        }
    }
    else {
        /* 没有 kind 字段，尝试作为单个 Pod 解析 */
        flb_plg_debug(ins, "No 'kind' field, attempting to parse as Pod");
        ret = parse_single_pod(&root, meta, ins);
    }

    msgpack_unpacked_destroy(&result);
    flb_free(buf);

    return ret;
}

/**
 * 从 Kubernetes API Server 获取 Pod 元数据
 * 使用 Pod UID 进行查询，支持缓存机制
 *
 * @param ctx 插件上下文
 * @param pod_uid Pod 的 UID
 * @param out_meta 输出参数，返回获取到的元数据
 * @return 成功返回 0，失败返回 -1
 */
int flb_pod_get_metadata(struct flb_filter_pod *ctx, const char *pod_uid,
                        struct flb_pod_meta **out_meta)
{
    struct flb_connection *conn = NULL;
    struct flb_http_client *client = NULL;
    struct flb_pod_meta *meta = NULL;
    char uri[1024];
    char auth_header[2048];
    int ret = -1;
    size_t resp_size;

    if (!ctx || !pod_uid || !out_meta) {
        return -1;
    }

    /* 检查缓存 */
    ret = flb_hash_table_get(ctx->pod_cache, pod_uid, strlen(pod_uid),
                            (void **)&meta, &resp_size);
    if (ret == 0 && meta) {
        /* 检查缓存是否过期 */
        if (time(NULL) - meta->cached_at < ctx->cache_ttl) {
            flb_plg_debug(ctx->ins, "Using cached metadata for pod_uid=%s", pod_uid);
            *out_meta = meta;
            return 0;
        }
        /* 缓存已过期，需要重新获取，但保留旧的元数据以防获取失败 */
        flb_plg_debug(ctx->ins, "Cache expired for pod_uid=%s, refreshing", pod_uid);
    }

    /* 创建新的元数据结构 */
    meta = flb_pod_meta_create();
    if (!meta) {
        flb_plg_error(ctx->ins, "Failed to create pod metadata structure");
        return -1;
    }

    meta->pod_id = flb_strdup(pod_uid);

    /* 构建 API 请求路径 - 使用 fieldSelector 通过 UID 查询 */
    snprintf(uri, sizeof(uri), FLB_POD_API_POD_BY_UID_PATH_FMT, pod_uid);

    flb_plg_debug(ctx->ins, "Requesting pod metadata from: %s:%d%s",
                 ctx->api_host, ctx->api_port, uri);

    /* 获取上游连接 */
    conn = flb_upstream_conn_get(ctx->upstream);
    if (!conn) {
        flb_plg_error(ctx->ins, "Failed to get upstream connection to %s:%d",
                     ctx->api_host, ctx->api_port);
        flb_pod_meta_destroy(meta);
        return -1;
    }

    /* 创建 HTTP 客户端 */
    client = flb_http_client(conn, FLB_HTTP_GET, uri,
                            NULL, 0,  /* 无请求体 */
                            ctx->api_host, ctx->api_port,
                            NULL, 0);

    if (!client) {
        flb_plg_error(ctx->ins, "Failed to create HTTP client");
        flb_upstream_conn_release(conn);
        flb_pod_meta_destroy(meta);
        return -1;
    }

    /* 配置 HTTP 客户端超时（10 秒） */
    flb_http_buffer_size(client, 4096);

    /* 添加认证 header */
    if (ctx->token && ctx->token_len > 0) {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", ctx->token);
        flb_http_add_header(client, "Authorization", 13,
                          auth_header, strlen(auth_header));
    }

    /* 添加标准 HTTP headers */
    flb_http_add_header(client, "User-Agent", 10, "Fluent-Bit", 10);
    flb_http_add_header(client, "Accept", 6, "application/json", 16);

    /* 发送 HTTP 请求 */
    ret = flb_http_do(client, &resp_size);

    if (ret != 0) {
        flb_plg_error(ctx->ins, "HTTP request failed: ret=%d", ret);
        flb_http_client_destroy(client);
        flb_upstream_conn_release(conn);
        flb_pod_meta_destroy(meta);
        return -1;
    }

    /* 检查 HTTP 响应状态 */
    if (client->resp.status != 200) {
        flb_plg_error(ctx->ins, "API request failed with status %d", client->resp.status);
        if (client->resp.payload_size > 0) {
            flb_plg_error(ctx->ins, "Error response: %.*s",
                         (int)client->resp.payload_size,
                         client->resp.payload);
        }
        flb_plg_error(ctx->ins, "Request URI was: %s", uri);

        /* 特殊处理常见错误 */
        if (client->resp.status == 401) {
            flb_plg_error(ctx->ins, "Authentication failed. Check token at %s", ctx->token_path);
        }
        else if (client->resp.status == 403) {
            flb_plg_error(ctx->ins, "Authorization failed. Pod may need RBAC permissions");
        }

        flb_http_client_destroy(client);
        flb_upstream_conn_release(conn);
        flb_pod_meta_destroy(meta);
        return -1;
    }

    flb_plg_debug(ctx->ins, "Received response: status=%d, size=%zu",
                 client->resp.status, client->resp.payload_size);

    /* 解析响应 */
    if (client->resp.payload_size > 0) {
        ret = parse_pod_response(client->resp.payload,
                                client->resp.payload_size,
                                meta, ctx->ins);
        if (ret != 0) {
            flb_plg_error(ctx->ins, "Failed to parse pod metadata response");
            flb_http_client_destroy(client);
            flb_upstream_conn_release(conn);
            flb_pod_meta_destroy(meta);
            return -1;
        }
    }
    else {
        flb_plg_error(ctx->ins, "Empty response from API server");
        flb_http_client_destroy(client);
        flb_upstream_conn_release(conn);
        flb_pod_meta_destroy(meta);
        return -1;
    }

    /* 清理 HTTP 客户端和连接 */
    flb_http_client_destroy(client);
    flb_upstream_conn_release(conn);

    /* 更新缓存 */
    flb_hash_table_add(ctx->pod_cache, pod_uid, strlen(pod_uid),
                      meta, sizeof(struct flb_pod_meta));

    *out_meta = meta;

    flb_plg_info(ctx->ins, "Successfully fetched metadata for pod %s: name=%s, namespace=%s, node=%s",
                pod_uid,
                meta->pod_name ? meta->pod_name : "unknown",
                meta->namespace ? meta->namespace : "unknown",
                meta->node_name ? meta->node_name : "unknown");

    return 0;
}