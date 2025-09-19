//
// Created by weipengyu on 25-9-19.
//

#ifndef FLB_FILTER_TEST_H
#define FLB_FILTER_TEST_H

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_filter.h>
#include <fluent-bit/flb_filter_plugin.h>

/* 插件上下文结构体 */
struct filter_test_ctx {
    struct flb_filter_instance *ins;  /* 插件实例 */
    char *add_field_key;              /* 要添加的字段名 */
    char *add_field_value;            /* 要添加的字段值 */
};

#endif
