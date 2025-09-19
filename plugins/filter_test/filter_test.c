//
// Created by weipengyu on 25-9-19.
//
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
#include <fluent-bit/flb_str.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_mem.h>
#include <fluent-bit/flb_kv.h>
#include <fluent-bit/flb_time.h>
#include <fluent-bit/flb_log_event_decoder.h>
#include <fluent-bit/flb_log_event_encoder.h>

#include <msgpack.h>
#include "filter_test.h"

#define PLUGIN_NAME "filter_test"

/* 初始化函数 - 必须实现 */
static int cb_test_init(struct flb_filter_instance *f_ins,
                        struct flb_config *config,
                        void *data)
{
    struct filter_test_ctx *ctx = NULL;

    /* 创建插件上下文 */
    ctx = flb_calloc(1, sizeof(struct filter_test_ctx));
    if (!ctx) {
        flb_plg_error(f_ins, "无法分配内存用于上下文");
        return -1;
    }

    ctx->ins = f_ins;

    /* 使用配置映射自动设置参数 */
    if (flb_filter_config_map_set(f_ins, ctx) < 0) {
        flb_plg_error(f_ins, "配置参数设置失败");
        flb_free(ctx);
        return -1;
    }

    /* 如果没有设置字段名，使用默认值 */
    if (!ctx->add_field_key) {
        ctx->add_field_key = flb_sds_create("test_field");
    }

    /* 如果没有设置字段值，使用默认值 */
    if (!ctx->add_field_value) {
        ctx->add_field_value = flb_sds_create("test_value");
    }

    flb_plg_info(f_ins, "Filter Test 插件初始化完成");
    flb_plg_info(f_ins, "将添加字段: %s = %s",
                 ctx->add_field_key, ctx->add_field_value);

    /* 将上下文设置到插件实例 */
    flb_filter_set_context(f_ins, ctx);

    return 0;
}

/* 过滤函数 - 必须实现 */
static int cb_test_filter(const void *data, size_t bytes,
                          const char *tag, int tag_len,
                          void **out_buf, size_t *out_size,
                          struct flb_filter_instance *f_ins,
                          struct flb_input_instance *i_ins,
                          void *context,
                          struct flb_config *config)
{
    struct filter_test_ctx *ctx = context;
    struct flb_log_event_decoder log_decoder;
    struct flb_log_event_encoder log_encoder;
    struct flb_log_event log_event;
    int ret;
    int modified = FLB_FALSE;

    (void) f_ins;
    (void) i_ins;
    (void) config;

    /* 初始化日志事件解码器 */
    ret = flb_log_event_decoder_init(&log_decoder, (char *) data, bytes);
    if (ret != FLB_EVENT_DECODER_SUCCESS) {
        flb_plg_error(ctx->ins, "日志事件解码器初始化失败: %d", ret);
        return FLB_FILTER_NOTOUCH;
    }

    /* 初始化日志事件编码器 */
    ret = flb_log_event_encoder_init(&log_encoder, FLB_LOG_EVENT_FORMAT_DEFAULT);
    if (ret != FLB_EVENT_ENCODER_SUCCESS) {
        flb_plg_error(ctx->ins, "日志事件编码器初始化失败: %d", ret);
        flb_log_event_decoder_destroy(&log_decoder);
        return FLB_FILTER_NOTOUCH;
    }

    /* 处理每条日志记录 */
    while ((ret = flb_log_event_decoder_next(&log_decoder, &log_event)) ==
           FLB_EVENT_DECODER_SUCCESS) {

        /* 开始新的日志记录 */
        ret = flb_log_event_encoder_begin_record(&log_encoder);
        if (ret != FLB_EVENT_ENCODER_SUCCESS) {
            break;
        }

        /* 设置时间戳 */
        ret = flb_log_event_encoder_set_timestamp(&log_encoder, &log_event.timestamp);
        if (ret != FLB_EVENT_ENCODER_SUCCESS) {
            break;
        }

        /* 设置元数据 */
        ret = flb_log_event_encoder_set_metadata_from_msgpack_object(
                &log_encoder, log_event.metadata);
        if (ret != FLB_EVENT_ENCODER_SUCCESS) {
            break;
        }

        /* 复制原始日志内容 */
        ret = flb_log_event_encoder_set_body_from_msgpack_object(
                &log_encoder, log_event.body);
        if (ret != FLB_EVENT_ENCODER_SUCCESS) {
            break;
        }

        /* 添加测试字段 */
        ret = flb_log_event_encoder_append_body_values(
                &log_encoder,
                FLB_LOG_EVENT_STRING_VALUE(ctx->add_field_key,
                                          flb_sds_len(ctx->add_field_key)),
                FLB_LOG_EVENT_STRING_VALUE(ctx->add_field_value,
                                          flb_sds_len(ctx->add_field_value)));
        if (ret != FLB_EVENT_ENCODER_SUCCESS) {
            break;
        }

        /* 提交记录 */
        ret = flb_log_event_encoder_commit_record(&log_encoder);
        if (ret != FLB_EVENT_ENCODER_SUCCESS) {
            break;
        }

        modified = FLB_TRUE;
    }

    /* 处理输出 */
    if (modified == FLB_TRUE && log_encoder.output_length > 0) {
        *out_buf = log_encoder.output_buffer;
        *out_size = log_encoder.output_length;

        /* 声明缓冲区所有权 */
        flb_log_event_encoder_claim_internal_buffer_ownership(&log_encoder);
        ret = FLB_FILTER_MODIFIED;
    } else {
        ret = FLB_FILTER_NOTOUCH;
    }

    /* 清理资源 */
    flb_log_event_decoder_destroy(&log_decoder);
    flb_log_event_encoder_destroy(&log_encoder);

    return ret;
}

/* 退出函数 - 必须实现 */
static int cb_test_exit(void *data, struct flb_config *config)
{
    struct filter_test_ctx *ctx = data;

    if (ctx != NULL) {
        /* 释放字符串资源 */
        if (ctx->add_field_key) {
            flb_sds_destroy(ctx->add_field_key);
        }
        if (ctx->add_field_value) {
            flb_sds_destroy(ctx->add_field_value);
        }

        /* 释放上下文 */
        flb_free(ctx);
    }

    return 0;
}

/* 配置参数映射表 */
static struct flb_config_map config_map[] = {
    {
        FLB_CONFIG_MAP_STR, "key", "test_field",
        0, FLB_TRUE, offsetof(struct filter_test_ctx, add_field_key),
        "要添加的字段名称"
    },
    {
        FLB_CONFIG_MAP_STR, "value", "test_value",
        0, FLB_TRUE, offsetof(struct filter_test_ctx, add_field_value),
        "要添加的字段值"
    },

    /* 配置结束标记 */
    {0}
};

/* 插件注册结构体 */
struct flb_filter_plugin filter_test_plugin = {
    .name         = "test",
    .description  = "Test filter plugin - 测试过滤器插件",
    .cb_init      = cb_test_init,
    .cb_filter    = cb_test_filter,
    .cb_exit      = cb_test_exit,
    .config_map   = config_map,
    .flags        = 0
};
