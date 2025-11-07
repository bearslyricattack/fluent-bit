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

/* 创建 pod 元数据结构 */
struct flb_pod_meta *flb_pod_meta_create(void)
{
    struct flb_pod_meta *meta;

    meta = flb_calloc(1, sizeof(struct flb_pod_meta));
    if (!meta) {
        flb_errno();
        return NULL;
    }

    /* 初始化哈希表存储 labels 和 annotations */
    meta->labels = flb_hash_table_create(FLB_HASH_TABLE_EVICT_NONE, 32, -1);
    if (!meta->labels) {
        flb_free(meta);
        return NULL;
    }

    meta->annotations = flb_hash_table_create(FLB_HASH_TABLE_EVICT_NONE, 32, -1);
    if (!meta->annotations) {
        flb_hash_table_destroy(meta->labels);
        flb_free(meta);
        return NULL;
    }

    meta->cached_at = time(NULL);
    return meta;
}

/* 销毁 pod 元数据结构 */
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

/* 解析 JSON 响应中的字符串字段 */
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
            strncmp(kv->key.via.str.ptr, key, kv->key.via.str.size) == 0) {

            val = &kv->val;
            if (val->type == MSGPACK_OBJECT_STR) {
                return flb_strndup(val->via.str.ptr, val->via.str.size);
            }
        }
    }

    return NULL;
}

/* 解析 JSON 响应中的 map 字段(labels/annotations) */
static int extract_json_map(msgpack_object *obj, const char *key,
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
            strncmp(kv->key.via.str.ptr, key, kv->key.via.str.size) == 0) {

            val = &kv->val;
            if (val->type == MSGPACK_OBJECT_MAP) {
                /* 遍历 labels 或 annotations */
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

/* 从 API 响应中解析 pod 元数据 */
static int parse_pod_metadata(const char *json_data, size_t json_len,
                             struct flb_pod_meta *meta,
                             struct flb_filter_instance *ins)
{
    int ret;
    int root_type;
    char *buf = NULL;
    size_t buf_size;
    msgpack_unpacked result;
    msgpack_object root;
    msgpack_object *metadata;
    msgpack_object *spec;
    int i;
    msgpack_object_kv *kv;

    /* 解析 JSON 为 msgpack */
    ret = flb_pack_json(json_data, json_len, &buf, &buf_size, &root_type, NULL);
    if (ret != 0) {
        flb_plg_error(ins, "failed to parse JSON response");
        return -1;
    }

    /* 解包 msgpack */
    msgpack_unpacked_init(&result);
    ret = msgpack_unpack_next(&result, buf, buf_size, NULL);
    if (ret != MSGPACK_UNPACK_SUCCESS) {
        flb_plg_error(ins, "failed to unpack msgpack data");
        flb_free(buf);
        return -1;
    }

    root = result.data;
    if (root.type != MSGPACK_OBJECT_MAP) {
        flb_plg_error(ins, "invalid response format");
        msgpack_unpacked_destroy(&result);
        flb_free(buf);
        return -1;
    }

    /* 提取 metadata 字段 */
    metadata = NULL;
    spec = NULL;

    for (i = 0; i < root.via.map.size; i++) {
        kv = &root.via.map.ptr[i];

        if (kv->key.type == MSGPACK_OBJECT_STR) {
            if (strncmp(kv->key.via.str.ptr, "metadata", 8) == 0) {
                metadata = &kv->val;
            }
            else if (strncmp(kv->key.via.str.ptr, "spec", 4) == 0) {
                spec = &kv->val;
            }
        }
    }

    /* 解析 metadata 字段 */
    if (metadata && metadata->type == MSGPACK_OBJECT_MAP) {
        /* 提取 pod name */
        meta->pod_name = extract_json_string(metadata, "name");

        /* 提取 namespace */
        meta->namespace = extract_json_string(metadata, "namespace");

        /* 提取 labels */
        extract_json_map(metadata, "labels", meta->labels);

        /* 提取 annotations */
        extract_json_map(metadata, "annotations", meta->annotations);

        flb_plg_debug(ins, "parsed pod metadata: name=%s, namespace=%s",
                     meta->pod_name ? meta->pod_name : "null",
                     meta->namespace ? meta->namespace : "null");
    }

    /* 解析 spec 字段获取 nodeName */
    if (spec && spec->type == MSGPACK_OBJECT_MAP) {
        meta->node_name = extract_json_string(spec, "nodeName");
        flb_plg_debug(ins, "parsed node name: %s",
                     meta->node_name ? meta->node_name : "null");
    }

    msgpack_unpacked_destroy(&result);
    flb_free(buf);

    return 0;
}

/* 从 Kubernetes API Server 获取 pod 元数据 */
int flb_pod_get_metadata(struct flb_filter_pod *ctx, const char *pod_id,
                        struct flb_pod_meta **out_meta)
{
    struct flb_connection *conn = NULL;
    struct flb_http_client *client = NULL;
    struct flb_pod_meta *meta = NULL;
    char uri[1024];
    char auth_header[2048];
    int ret = -1;
    size_t resp_size;
    char *namespace = "default";  /* TODO: 从配置或环境变量获取 */

    if (!ctx || !pod_id || !out_meta) {
        return -1;
    }

    /* 首先检查缓存 */
    ret = flb_hash_table_get(ctx->pod_cache, pod_id, strlen(pod_id),
                            (void **)&meta, &resp_size);
    if (ret == 0 && meta) {
        /* 检查缓存是否过期 */
        if (time(NULL) - meta->cached_at < ctx->cache_ttl) {
            flb_plg_debug(ctx->ins, "using cached metadata for pod_id=%s", pod_id);
            *out_meta = meta;
            return 0;
        }
        /* 缓存已过期,需要重新获取 */
        flb_plg_debug(ctx->ins, "cache expired for pod_id=%s", pod_id);
    }

    /* 创建新的元数据结构 */
    meta = flb_pod_meta_create();
    if (!meta) {
        flb_plg_error(ctx->ins, "failed to create pod metadata structure");
        return -1;
    }

    meta->pod_id = flb_strdup(pod_id);

    /* 构建 API 请求路径 */
    snprintf(uri, sizeof(uri), FLB_POD_API_POD_PATH_FMT, namespace, pod_id);

    /* 获取连接 */
    conn = flb_upstream_conn_get(ctx->upstream);
    if (!conn) {
        flb_plg_error(ctx->ins, "failed to get upstream connection");
        flb_pod_meta_destroy(meta);
        return -1;
    }

    /* 创建 HTTP 客户端 */
    client = flb_http_client(conn, FLB_HTTP_GET, uri,
                            NULL, 0,
                            ctx->api_host, ctx->api_port,
                            NULL, 0);
    if (!client) {
        flb_plg_error(ctx->ins, "failed to create HTTP client");
        flb_upstream_conn_release(conn);
        flb_pod_meta_destroy(meta);
        return -1;
    }

    /* 添加认证 header */
    if (ctx->token && ctx->token_len > 0) {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", ctx->token);
        flb_http_add_header(client, "Authorization", 13,
                          auth_header, strlen(auth_header));
    }

    /* 添加标准 HTTP headers */
    flb_http_add_header(client, "User-Agent", 10,
                       "Fluent-Bit", 10);
    flb_http_add_header(client, "Accept", 6,
                       "application/json", 16);

    flb_plg_debug(ctx->ins, "requesting pod metadata: %s", uri);

    /* 发送 HTTP 请求 */
    ret = flb_http_do(client, &resp_size);

    if (ret != 0 || client->resp.status != 200) {
        flb_plg_error(ctx->ins, "HTTP request failed: ret=%d, status=%d",
                     ret, client->resp.status);

        /* 打印错误响应内容用于调试 */
        if (client->resp.payload_size > 0) {
            flb_plg_debug(ctx->ins, "error response: %.*s",
                         (int)client->resp.payload_size,
                         client->resp.payload);
        }

        flb_http_client_destroy(client);
        flb_upstream_conn_release(conn);
        flb_pod_meta_destroy(meta);
        return -1;
    }

    flb_plg_debug(ctx->ins, "received response: status=%d, size=%zu",
                 client->resp.status, client->resp.payload_size);

    /* 解析响应 */
    if (client->resp.payload_size > 0) {
        ret = parse_pod_metadata(client->resp.payload,
                                client->resp.payload_size,
                                meta, ctx->ins);
        if (ret != 0) {
            flb_plg_error(ctx->ins, "failed to parse pod metadata");
            flb_http_client_destroy(client);
            flb_upstream_conn_release(conn);
            flb_pod_meta_destroy(meta);
            return -1;
        }
    }

    /* 清理 HTTP 资源 */
    flb_http_client_destroy(client);
    flb_upstream_conn_release(conn);

    /* 缓存元数据 */
    flb_hash_table_add(ctx->pod_cache, pod_id, strlen(pod_id),
                      meta, sizeof(struct flb_pod_meta));

    *out_meta = meta;

    flb_plg_info(ctx->ins, "successfully fetched metadata for pod %s: name=%s, namespace=%s",
                pod_id,
                meta->pod_name ? meta->pod_name : "unknown",
                meta->namespace ? meta->namespace : "unknown");

    return 0;
}
