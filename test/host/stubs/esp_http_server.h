// Host stub: the esp_http_server API used by clu_ota.c (functions defined by the test).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "esp_err.h"
typedef void *httpd_handle_t;
typedef enum { HTTP_GET = 1, HTTP_POST = 3, HTTP_OPTIONS = 6 } httpd_method_t;
typedef struct httpd_req {
    size_t content_len;
} httpd_req_t;
typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;
typedef struct {
    uint16_t server_port, ctrl_port, max_open_sockets, max_uri_handlers, recv_wait_timeout, send_wait_timeout;
    bool lru_purge_enable;
    size_t stack_size;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() {.server_port = 80, .ctrl_port = 32768, .max_open_sockets = 7, \
                                .max_uri_handlers = 8, .recv_wait_timeout = 5, .send_wait_timeout = 5, \
                                .stack_size = 4096}
#define HTTPD_SOCK_ERR_FAIL -1
#define HTTPD_SOCK_ERR_TIMEOUT -3
#define HTTPD_RESP_USE_STRLEN -1
esp_err_t httpd_start(httpd_handle_t *h, const httpd_config_t *c);
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t len);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
