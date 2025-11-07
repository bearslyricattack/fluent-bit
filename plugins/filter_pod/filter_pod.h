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

#ifndef FLB_FILTER_POD_H
#define FLB_FILTER_POD_H

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_filter.h>
#include <fluent-bit/flb_hash_table.h>
#include <fluent-bit/flb_upstream.h>
#include <fluent-bit/tls/flb_tls.h>

/*
 * Kubernetes API endpoints
 *
 * 使用 fieldSelector 查询 Pod，这是推荐的方式，因为：
 * 1. 可以通过 UID 精确查询（Pod UID 是唯一的）
 * 2. 返回 PodList 格式，易于处理
 * 3. 支持跨 namespace 查询
 */

/* 使用 fieldSelector 通过 UID 查询 Pod，返回 PodList 格式 */
#define FLB_POD_API_POD_BY_UID_PATH_FMT "/api/v1/pods?fieldSelector=metadata.uid=%s"

/* 在特定 namespace 中通过 UID 查询 Pod（如果已知 namespace 可提高性能） */
#define FLB_POD_API_POD_BY_UID_NS_PATH_FMT "/api/v1/namespaces/%s/pods?fieldSelector=metadata.uid=%s"

/* 查询 namespace 信息（未使用，保留供将来扩展） */
#define FLB_POD_API_NAMESPACE_PATH_FMT "/api/v1/namespaces/%s"

/*
 * 默认配置值
 * 这些值适用于在 Kubernetes 集群内运行的 Pod
 */
#define FLB_POD_DEFAULT_API_HOST "kubernetes.default.svc"  /* K8s 集群内部 DNS */
#define FLB_POD_DEFAULT_API_PORT 443                       /* HTTPS 端口 */
#define FLB_POD_DEFAULT_TOKEN_PATH "/var/run/secrets/kubernetes.io/serviceaccount/token"
#define FLB_POD_DEFAULT_CA_PATH "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt"
#define FLB_POD_DEFAULT_NAMESPACE_PATH "/var/run/secrets/kubernetes.io/serviceaccount/namespace"

/* 缓存设置 */
#define FLB_POD_CACHE_TTL 3600  /* 缓存 TTL：1 小时（秒） */

/**
 * Pod 元数据结构
 * 存储从 Kubernetes API 获取的 Pod 信息
 */
struct flb_pod_meta {
    char *pod_id;                        /* Pod UID（唯一标识符）*/
    char *pod_name;                      /* Pod 名称 */
    char *namespace;                     /* Pod 所在的 namespace */
    char *node_name;                     /* Pod 运行的节点名称 */
    struct flb_hash_table *labels;       /* Pod labels（键值对）*/
    struct flb_hash_table *annotations;  /* Pod annotations（键值对）*/
    time_t cached_at;                    /* 缓存时间戳，用于 TTL 检查 */
};

/**
 * 插件上下文结构
 * 包含插件运行所需的所有配置和状态信息
 */
struct flb_filter_pod {
    /* Kubernetes API 配置 */
    char *api_host;                      /* API Server 主机名 */
    int api_port;                        /* API Server 端口 */
    int use_tls;                         /* 是否使用 TLS/HTTPS */
    char *token_path;                    /* ServiceAccount token 文件路径 */
    char *ca_path;                       /* CA 证书文件路径 */
    char *namespace_path;                /* Namespace 文件路径（未使用）*/
    char *token;                         /* 已加载的 token 内容 */
    size_t token_len;                    /* Token 长度 */

    /* TLS 配置 */
    int tls_verify;                      /* TLS 证书验证 */
    int tls_verify_hostname;             /* TLS 主机名验证 */
    int tls_debug;                       /* TLS 调试级别 */
    char *tls_vhost;                     /* TLS 虚拟主机名 (SNI) */
    char *tls_ca_file;                   /* TLS CA 证书文件 */
    char *tls_ca_path;                   /* TLS CA 证书目录 */
    struct flb_tls *tls;                 /* TLS 上下文 */

    /* Pod ID 字段配置 */
    char *pod_id_field;                  /* 日志记录中包含 Pod UID 的字段名 */

    /* 元数据丰富选项 */
    int add_labels;                      /* 是否添加 Pod labels */
    int add_annotations;                 /* 是否添加 Pod annotations */
    int add_namespace;                   /* 是否添加 namespace */
    int add_pod_name;                    /* 是否添加 Pod 名称 */
    int add_node_name;                   /* 是否添加节点名称 */

    /* 网络连接 */
    struct flb_upstream *upstream;       /* 到 API Server 的 upstream 连接 */

    /* 元数据缓存 */
    struct flb_hash_table *pod_cache;    /* Pod 元数据缓存（key: pod_uid）*/
    int cache_ttl;                       /* 缓存 TTL（秒）*/

    /* 插件实例 */
    struct flb_filter_instance *ins;     /* 过滤器实例 */
    struct flb_config *config;           /* Fluent Bit 配置 */
};

/**
 * 函数原型
 */

/**
 * 创建一个新的 Pod 元数据结构
 * @return 新创建的元数据结构，失败返回 NULL
 */
struct flb_pod_meta *flb_pod_meta_create(void);

/**
 * 销毁 Pod 元数据结构并释放所有资源
 * @param meta 要销毁的元数据结构
 */
void flb_pod_meta_destroy(struct flb_pod_meta *meta);

/**
 * 从 Kubernetes API Server 获取 Pod 元数据（支持缓存）
 * @param ctx 插件上下文
 * @param pod_id Pod UID
 * @param out_meta 输出参数，返回获取到的元数据
 * @return 成功返回 0，失败返回 -1
 */
int flb_pod_get_metadata(struct flb_filter_pod *ctx, const char *pod_id,
                         struct flb_pod_meta **out_meta);

#endif /* FLB_FILTER_POD_H */