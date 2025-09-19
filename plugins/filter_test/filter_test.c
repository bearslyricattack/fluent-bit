#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_filter.h>
#include <fluent-bit/flb_filter_plugin.h>
#include <fluent-bit/flb_config.h>
#include <fluent-bit/flb_mem.h>
#include <fluent-bit/flb_log_event_decoder.h>
#include <fluent-bit/flb_log_event_encoder.h>
#include <msgpack.h>
#include "filter_test.h"

/* 打印 msgpack 对象内容 */
static void print_msgpack_object(msgpack_object *obj, int depth)
{
    int i;

    /* 缩进 */
    for (i = 0; i < depth; i++) {
        printf("  ");
    }

    switch (obj->type) {
        case MSGPACK_OBJECT_NIL:
            printf("null\n");
            break;

        case MSGPACK_OBJECT_BOOLEAN:
            printf("%s\n", obj->via.boolean ? "true" : "false");
            break;

        case MSGPACK_OBJECT_POSITIVE_INTEGER:
            printf("%llu\n", obj->via.u64);
            break;

        case MSGPACK_OBJECT_NEGATIVE_INTEGER:
            printf("%lld\n", obj->via.i64);
            break;

        case MSGPACK_OBJECT_FLOAT:
            printf("%f\n", obj->via.f64);
            break;

        case MSGPACK_OBJECT_STR:
            printf("\"%.*s\"\n", obj->via.str.size, obj->via.str.ptr);
            break;

        case MSGPACK_OBJECT_BIN:
            printf("binary[%d]\n", obj->via.bin.size);
            break;

        case MSGPACK_OBJECT_ARRAY:
            printf("array[%d]:\n", obj->via.array.size);
            for (i = 0; i < obj->via.array.size; i++) {
                print_msgpack_object(&obj->via.array.ptr[i], depth + 1);
            }
            break;

        case MSGPACK_OBJECT_MAP:
            printf("map[%d]:\n", obj->via.map.size);
            for (i = 0; i < obj->via.map.size; i++) {
                /* 打印键 */
                for (int j = 0; j <= depth; j++) printf("  ");
                printf("key: ");
                print_msgpack_object(&obj->via.map.ptr[i].key, 0);

                /* 打印值 */
                for (int j = 0; j <= depth; j++) printf("  ");
                printf("val: ");
                print_msgpack_object(&obj->via.map.ptr[i].val, 0);
            }
            break;

        default:
            printf("unknown type\n");
            break;
    }
}

/* 初始化函数 */
static int cb_test_init(struct flb_filter_instance *f_ins,
                        struct flb_config *config,
                        void *data)
{
    struct filter_test_ctx *ctx;

    ctx = flb_calloc(1, sizeof(struct filter_test_ctx));
    if (!ctx) {
        return -1;
    }

    ctx->ins = f_ins;
    flb_filter_set_context(f_ins, ctx);

    printf("=== Filter Test 插件初始化成功 ===\n");
    return 0;
}

/* 过滤函数 - 输出日志目录信息 */
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
    struct flb_log_event log_event;
    int ret;
    int record_count = 0;

    printf("\n========================================\n");
    printf("Filter Test - 日志目录信息\n");
    printf("Tag: %.*s\n", tag_len, tag);
    printf("Data Size: %zu bytes\n", bytes);
    printf("========================================\n");

    /* 初始化解码器 */
    ret = flb_log_event_decoder_init(&log_decoder, (char *) data, bytes);
    if (ret != FLB_EVENT_DECODER_SUCCESS) {
        printf("解码器初始化失败\n");
        return FLB_FILTER_NOTOUCH;
    }

    /* 遍历每条日志记录 */
    while ((ret = flb_log_event_decoder_next(&log_decoder, &log_event)) == FLB_EVENT_DECODER_SUCCESS) {

        record_count++;
        printf("\n--- 记录 #%d ---\n", record_count);

        /* 打印时间戳信息 */
        printf("时间戳: %ld.%09ld\n",
               log_event.timestamp.tm.tv_sec,
               log_event.timestamp.tm.tv_nsec);

        /* 打印元数据 */
        if (log_event.metadata && log_event.metadata->type != MSGPACK_OBJECT_NIL) {
            printf("元数据:\n");
            print_msgpack_object(log_event.metadata, 1);
        }

        /* 打印日志内容 */
        printf("日志内容:\n");
        if (log_event.body) {
            print_msgpack_object(log_event.body, 1);
        } else {
            printf("  (空)\n");
        }
    }

    printf("\n总共处理了 %d 条记录\n", record_count);
    printf("========================================\n");

    /* 清理解码器 */
    flb_log_event_decoder_destroy(&log_decoder);

    /* 不修改数据，直接传递 */
    return FLB_FILTER_NOTOUCH;
}

/* 退出函数 */
static int cb_test_exit(void *data, struct flb_config *config)
{
    struct filter_test_ctx *ctx = data;

    if (ctx) {
        flb_free(ctx);
    }

    printf("=== Filter Test 插件已退出 ===\n");
    return 0;
}

/* 插件注册 */
struct flb_filter_plugin filter_test_plugin = {
    .name         = "test",
    .description  = "Test filter - 输出日志目录信息",
    .cb_init      = cb_test_init,
    .cb_filter    = cb_test_filter,
    .cb_exit      = cb_test_exit,
    .flags        = 0
};
