#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_filter.h>
#include <fluent-bit/flb_filter_plugin.h>
#include <fluent-bit/flb_mem.h>
#include <fluent-bit/flb_str.h>
#include <fluent-bit/flb_time.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_http_client.h>
#include <fluent-bit/flb_upstream.h>
#include <msgpack.h>
#include <unistd.h>

struct flb_filter_test {
    struct flb_upstream *upstream;
    char *api_host;
    int api_port;
    char *api_path;
};

static int cb_test_init(struct flb_filter_instance *f_ins,
                       struct flb_config *config,
                       void *data)
{
    struct flb_filter_test *ctx;

    flb_plg_info(f_ins, "插件初始化开始");

    // 分配上下文
    ctx = flb_calloc(1, sizeof(struct flb_filter_test));
    if (!ctx) {
        flb_plg_error(f_ins, "无法分配内存");
        return -1;
    }

    ctx->api_host = flb_strdup("httpbin.org");
    ctx->api_port = 80;
    ctx->api_path = flb_strdup("/uuid");

    // 创建upstream连接
    ctx->upstream = flb_upstream_create(config, ctx->api_host, ctx->api_port,
                                       FLB_IO_TCP, NULL);
    if (!ctx->upstream) {
        flb_plg_error(f_ins, "无法创建upstream连接");
        flb_free(ctx->api_host);
        flb_free(ctx->api_path);
        flb_free(ctx);
        return -1;
    }

    // 设置插件上下文
    flb_filter_set_context(f_ins, ctx);

    flb_plg_info(f_ins, "插件初始化成功，API: %s:%d%s",
                 ctx->api_host, ctx->api_port, ctx->api_path);

    return 0;
}

static int make_http_request(struct flb_filter_instance *f_ins,
                           struct flb_filter_test *ctx,
                           char **response_data, size_t *response_len,
                           int *status_code)
{
    struct flb_http_client *client;
    struct flb_connection *conn;
    int ret = -1;
    size_t bytes_received = 0;

    *status_code = 0;

    // 获取连接
    conn = flb_upstream_conn_get(ctx->upstream);
    if (!conn) {
        flb_plg_error(f_ins, "无法获取upstream连接");
        return -1;
    }

    // 创建HTTP客户端
    client = flb_http_client(conn, FLB_HTTP_GET, ctx->api_path,
                            NULL, 0, ctx->api_host, ctx->api_port, NULL, 0);
    if (!client) {
        flb_plg_error(f_ins, "无法创建HTTP客户端");
        flb_upstream_conn_release(conn);
        return -1;
    }

    // 添加标准HTTP头
    flb_http_add_header(client, "Host", 4, ctx->api_host, strlen(ctx->api_host));
    flb_http_add_header(client, "User-Agent", 10, "Mozilla/5.0 (compatible; fluent-bit)", 35);
    flb_http_add_header(client, "Accept", 6, "text/html,*/*", 13);
    flb_http_add_header(client, "Connection", 10, "close", 5);

    flb_plg_info(f_ins, "发送HTTP请求到: %s:%d%s", ctx->api_host, ctx->api_port, ctx->api_path);

    // 发送请求
    ret = flb_http_do(client, &bytes_received);

    // 记录状态码
    *status_code = client->resp.status;

    flb_plg_info(f_ins, "HTTP请求完成，返回码: %d, 状态码: %d, 接收字节: %zu",
                 ret, client->resp.status, bytes_received);

    if (ret != 0) {
        flb_plg_error(f_ins, "HTTP请求失败，错误码: %d", ret);
        goto cleanup;
    }

    // 检查响应状态 - 包含重定向作为成功
    if ((client->resp.status >= 200 && client->resp.status < 300) ||
        (client->resp.status >= 300 && client->resp.status < 400)) {

        // 成功响应或重定向
        if (client->resp.payload_size > 0) {
            *response_data = flb_malloc(client->resp.payload_size + 1);
            if (*response_data) {
                memcpy(*response_data, client->resp.payload, client->resp.payload_size);
                (*response_data)[client->resp.payload_size] = '\0';
                *response_len = client->resp.payload_size;
                ret = 0;

                if (client->resp.status >= 300 && client->resp.status < 400) {
                    flb_plg_info(f_ins, "HTTP重定向响应，状态码: %d, 响应大小: %zu bytes",
                                client->resp.status, *response_len);
                } else {
                    flb_plg_info(f_ins, "HTTP请求成功，响应大小: %zu bytes", *response_len);
                }

                // 打印响应的前100个字符用于调试
                char preview[101];
                size_t preview_len = (*response_len > 100) ? 100 : *response_len;
                memcpy(preview, *response_data, preview_len);
                preview[preview_len] = '\0';
                flb_plg_info(f_ins, "响应预览: %s", preview);
            } else {
                flb_plg_error(f_ins, "无法分配响应数据内存");
                ret = -1;
            }
        } else {
            flb_plg_info(f_ins, "HTTP请求成功但无响应数据，状态码: %d", client->resp.status);
            ret = 0;
        }
    } else {
        flb_plg_warn(f_ins, "HTTP响应状态异常: %d", client->resp.status);
        // 即使状态码异常，也尝试读取响应内容用于调试
        if (client->resp.payload_size > 0) {
            char *error_response = flb_malloc(client->resp.payload_size + 1);
            if (error_response) {
                memcpy(error_response, client->resp.payload, client->resp.payload_size);
                error_response[client->resp.payload_size] = '\0';
                flb_plg_warn(f_ins, "错误响应内容: %.200s", error_response);
                flb_free(error_response);
            }
        }
        ret = -1;
    }

cleanup:
    flb_http_client_destroy(client);
    flb_upstream_conn_release(conn);
    return ret;
}

static int cb_test_filter(const void *data, size_t bytes,
                         const char *tag, int tag_len,
                         void **out_buf, size_t *out_bytes,
                         struct flb_filter_instance *f_ins,
                         struct flb_input_instance *i_ins,
                         void *filter_context,
                         struct flb_config *config)
{
    struct flb_filter_test *ctx = filter_context;
    msgpack_unpacker result;
    msgpack_unpacked record;
    msgpack_sbuffer tmp_sbuf;
    msgpack_packer tmp_packer;
    int modified = FLB_FALSE;
    int ret = FLB_FILTER_NOTOUCH;
    char *response_data = NULL;
    size_t response_len = 0;
    int status_code = 0;  // 声明 status_code 变量

    flb_plg_info(f_ins, "开始处理日志记录...");

    // 发起HTTP请求 - 修复函数调用，添加缺少的参数
    int http_result = make_http_request(f_ins, ctx, &response_data, &response_len, &status_code);

    // 初始化msgpack
    msgpack_sbuffer_init(&tmp_sbuf);
    msgpack_packer_init(&tmp_packer, &tmp_sbuf, msgpack_sbuffer_write);

    msgpack_unpacker_init(&result, 1024);
    msgpack_unpacker_reserve_buffer(&result, bytes);
    memcpy(msgpack_unpacker_buffer(&result), data, bytes);
    msgpack_unpacker_buffer_consumed(&result, bytes);

    msgpack_unpacked_init(&record);
    while (msgpack_unpacker_next(&result, &record) == MSGPACK_UNPACK_SUCCESS) {
        if (record.data.type == MSGPACK_OBJECT_ARRAY &&
            record.data.via.array.size == 2) {

            msgpack_object *timestamp = &record.data.via.array.ptr[0];
            msgpack_object *log_record = &record.data.via.array.ptr[1];

            if (log_record->type == MSGPACK_OBJECT_MAP) {
                // 打包时间戳
                msgpack_pack_array(&tmp_packer, 2);
                msgpack_pack_object(&tmp_packer, *timestamp);

                // 创建新的map，添加额外字段
                msgpack_pack_map(&tmp_packer, log_record->via.map.size + 3);

                // 复制原有字段
                for (int i = 0; i < log_record->via.map.size; i++) {
                    msgpack_pack_object(&tmp_packer, log_record->via.map.ptr[i].key);
                    msgpack_pack_object(&tmp_packer, log_record->via.map.ptr[i].val);
                }

                // 添加当前工作目录
                char cwd[1024];
                if (getcwd(cwd, sizeof(cwd))) {
                    msgpack_pack_str(&tmp_packer, 11);
                    msgpack_pack_str_body(&tmp_packer, "current_dir", 11);
                    msgpack_pack_str(&tmp_packer, strlen(cwd));
                    msgpack_pack_str_body(&tmp_packer, cwd, strlen(cwd));
                }

                // 添加HTTP状态码
                msgpack_pack_str(&tmp_packer, 16);
                msgpack_pack_str_body(&tmp_packer, "http_status_code", 16);
                msgpack_pack_int(&tmp_packer, status_code);

                // 添加HTTP响应状态
                msgpack_pack_str(&tmp_packer, 11);
                msgpack_pack_str_body(&tmp_packer, "http_status", 11);
                if (http_result == 0) {
                    // 包括2xx和3xx都算成功
                    if (status_code >= 200 && status_code < 400) {
                        msgpack_pack_str(&tmp_packer, 7);
                        msgpack_pack_str_body(&tmp_packer, "success", 7);
                    } else {
                        char status_msg[64];
                        snprintf(status_msg, sizeof(status_msg), "completed_%d", status_code);
                        msgpack_pack_str(&tmp_packer, strlen(status_msg));
                        msgpack_pack_str_body(&tmp_packer, status_msg, strlen(status_msg));
                    }
                } else {
                    char status_msg[64];
                    snprintf(status_msg, sizeof(status_msg), "failed_%d", status_code);
                    msgpack_pack_str(&tmp_packer, strlen(status_msg));
                    msgpack_pack_str_body(&tmp_packer, status_msg, strlen(status_msg));
                }

                modified = FLB_TRUE;
            }
        }
    }

    // 清理
    msgpack_unpacked_destroy(&record);
    msgpack_unpacker_destroy(&result);

    if (response_data) {
        flb_free(response_data);
    }

    if (modified) {
        *out_buf = flb_malloc(tmp_sbuf.size);
        if (*out_buf) {
            memcpy(*out_buf, tmp_sbuf.data, tmp_sbuf.size);
            *out_bytes = tmp_sbuf.size;
            ret = FLB_FILTER_MODIFIED;
            flb_plg_info(f_ins, "日志记录已修改，添加了网络请求信息");
        }
    }

    msgpack_sbuffer_destroy(&tmp_sbuf);
    return ret;
}

static int cb_test_exit(void *data, struct flb_config *config)
{
    struct flb_filter_test *ctx = data;

    if (ctx) {
        if (ctx->upstream) {
            flb_upstream_destroy(ctx->upstream);
        }
        if (ctx->api_host) {
            flb_free(ctx->api_host);
        }
        if (ctx->api_path) {
            flb_free(ctx->api_path);
        }
        flb_free(ctx);
    }

    return 0;
}

struct flb_filter_plugin filter_test_plugin = {
    .name         = "test",
    .description  = "Test filter with HTTP requests - 输出日志目录信息和网络请求状态",
    .cb_init      = cb_test_init,
    .cb_filter    = cb_test_filter,
    .cb_exit      = cb_test_exit,
    .flags        = 0
};
