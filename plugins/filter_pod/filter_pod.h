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

/* Kubernetes API endpoints */
#define FLB_POD_API_POD_PATH_FMT "/api/v1/pods?fieldSelector=metadata.uid=%s"
#define FLB_POD_API_NAMESPACE_PATH_FMT "/api/v1/namespaces/%s"

/* Default values */
#define FLB_POD_DEFAULT_API_HOST "kubernetes.default.svc"
#define FLB_POD_DEFAULT_API_PORT 443
#define FLB_POD_DEFAULT_TOKEN_PATH "/var/run/secrets/kubernetes.io/serviceaccount/token"
#define FLB_POD_DEFAULT_CA_PATH "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt"
#define FLB_POD_DEFAULT_NAMESPACE_PATH "/var/run/secrets/kubernetes.io/serviceaccount/namespace"

/* Cache settings */
#define FLB_POD_CACHE_TTL 3600  /* 1 hour cache TTL */

/* Pod metadata structure */
struct flb_pod_meta {
    char *pod_id;
    char *pod_name;
    char *namespace;
    char *node_name;
    struct flb_hash_table *labels;
    struct flb_hash_table *annotations;
    time_t cached_at;
};

/* Plugin context structure */
struct flb_filter_pod {
    /* Kubernetes API configuration */
    char *api_host;
    int api_port;
    int use_tls;
    char *token_path;
    char *ca_path;
    char *namespace_path;
    char *token;
    size_t token_len;

    /* Pod ID field configuration */
    char *pod_id_field;  /* Field name containing pod ID in the log record */

    /* Metadata enrichment options */
    int add_labels;
    int add_annotations;
    int add_namespace;
    int add_pod_name;
    int add_node_name;

    /* Networking */
    struct flb_upstream *upstream;

    /* Metadata cache */
    struct flb_hash_table *pod_cache;
    int cache_ttl;

    /* Plugin instance */
    struct flb_filter_instance *ins;
    struct flb_config *config;
};

/* Function prototypes */
struct flb_pod_meta *flb_pod_meta_create(void);
void flb_pod_meta_destroy(struct flb_pod_meta *meta);
int flb_pod_get_metadata(struct flb_filter_pod *ctx, const char *pod_id,
                         struct flb_pod_meta **out_meta);

#endif /* FLB_FILTER_POD_H */