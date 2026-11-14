/**
 * @file clu_http.c
 * @brief GATE HTTP objects, C side: outgoing requests (worker task), incoming requests (server
 *        task) and the Lua bindings used by http.lua.
 *
 * Threading: the Lua task only touches the two queues and the pending-request slot (under s_lock);
 * the worker and the server never call into Lua.
 */
#include "clu_http.h"

#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_proto.h"
#include "lwip/sockets.h"

static const char *TAG = "http";

extern const char http_lua_start[] asm("_binary_http_lua_start");
extern const char http_lua_end[] asm("_binary_http_lua_end");

#define JOB_QUEUE     4     // outgoing requests waiting for the worker
#define MSG_QUEUE     8     // results / incoming requests waiting for the Lua task
#define HEAD_MAX      4096  // request head (request line + headers), response headers
#define RECV_TIMEOUT  5     // s: idle client connection
#define ABORTED_MAX   8
#define WORKER_STACK  (10 * 1024) // esp_http_client + TLS handshake
#define SERVER_STACK  (4 * 1024)

static clu_http_cfg_t s_cfg;
static QueueHandle_t s_jobs;
static QueueHandle_t s_msgs;
static SemaphoreHandle_t s_lock;
static uint32_t s_next_id; // under s_lock
static bool s_started;

/** @brief Outgoing request for the worker. */
typedef struct {
    uint32_t id;
    int method; // esp_http_client_method_t
    int timeout_ms;
    char *url;
    char *headers; // "Name: value\r\n" lines
    char *body;
    size_t body_len;
} job_t;

enum { MSG_RESP, MSG_REQ };

/** @brief Message for the Lua task: response to a job, or an incoming request. */
typedef struct {
    int kind;
    uint32_t id;
    int status;     // MSG_RESP: HTTP status, -1 = no response (connection / timeout / too large)
    char *method;   // MSG_REQ
    char *path;     // MSG_REQ
    char *query;    // MSG_REQ
    char *headers;  // both
    char *body;     // both
    size_t body_len;
    char peer[16];  // MSG_REQ
} msg_t;

/** @brief The incoming request waiting for SendResponse (one at a time: the server is serial). */
static struct {
    uint32_t rid; // 0 = none
    bool done;
    int status;
    char *headers;
    char *body;
    size_t body_len;
} s_wait;
static SemaphoreHandle_t s_wait_sem;

static uint32_t s_aborted[ABORTED_MAX]; // under s_lock
static int s_aborted_pos;

static void msg_free(msg_t *message)
{
    if (!message) {
        return;
    }
    free(message->method);
    free(message->path);
    free(message->query);
    free(message->headers);
    free(message->body);
    free(message);
}

static void job_free(job_t *job)
{
    free(job->url);
    free(job->headers);
    free(job->body);
    free(job);
}

static uint32_t next_id(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (++s_next_id == 0) {
        s_next_id = 1;
    }
    uint32_t id = s_next_id;
    xSemaphoreGive(s_lock);
    return id;
}

static bool is_aborted(uint32_t id)
{
    bool aborted = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < ABORTED_MAX; i++) {
        aborted |= s_aborted[i] == id;
    }
    xSemaphoreGive(s_lock);
    return aborted;
}

/** @brief Hand @p m to the Lua task; freed if the queue stays full. */
static bool post(msg_t *message)
{
    if (xQueueSend(s_msgs, &message, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "queue to Lua full: %s %lu dropped", message->kind == MSG_REQ ? "request" : "response",
                 (unsigned long)message->id);
        msg_free(message);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Outgoing requests (HttpRequest)
// ---------------------------------------------------------------------------
/** @brief Growing buffer. */
typedef struct {
    char *p;
    size_t len, cap, max;
    bool overflow;
} buf_t;

static void buf_add(buf_t *buffer, const char *source, size_t source_length)
{
    if (buffer->overflow || buffer->len + source_length > buffer->max) {
        buffer->overflow = true;
        return;
    }
    if (buffer->len + source_length + 1 > buffer->cap) {
        size_t cap = buffer->cap ? buffer->cap : 256;
        while (cap < buffer->len + source_length + 1) {
            cap *= 2;
        }
        char *resized_data = realloc(buffer->p, cap);
        if (!resized_data) {
            buffer->overflow = true;
            return;
        }
        buffer->p = resized_data;
        buffer->cap = cap;
    }
    memcpy(buffer->p + buffer->len, source, source_length);
    buffer->len += source_length;
    buffer->p[buffer->len] = 0;
}

typedef struct {
    buf_t hdr, body;
} acc_t;

static esp_err_t on_client_event(esp_http_client_event_t *event)
{
    acc_t *accumulator = event->user_data;
    switch (event->event_id) {
    case HTTP_EVENT_HEADERS_SENT: // (again) after a redirect: keep the final response only
        accumulator->hdr.len = accumulator->body.len = 0;
        accumulator->hdr.overflow = accumulator->body.overflow = false;
        break;
    case HTTP_EVENT_ON_HEADER:
        buf_add(&accumulator->hdr, event->header_key, strlen(event->header_key));
        buf_add(&accumulator->hdr, ": ", 2);
        buf_add(&accumulator->hdr, event->header_value, strlen(event->header_value));
        buf_add(&accumulator->hdr, "\r\n", 2);
        break;
    case HTTP_EVENT_ON_DATA: buf_add(&accumulator->body, event->data, (size_t)event->data_len); break;
    default: break;
    }
    return ESP_OK;
}

/** @brief Apply "Name: value\r\n" lines to the client. */
static void set_headers(esp_http_client_handle_t http_client, char *header_lines)
{
    for (char *line = header_lines; line && *line;) {
        char *eol = strstr(line, "\r\n");
        if (eol) {
            *eol = 0;
        }
        char *colon = strchr(line, ':');
        if (colon && colon > line) {
            *colon = 0;
            char *trimmed_value = colon + 1;
            while (*trimmed_value == ' ') {
                trimmed_value++;
            }
            esp_http_client_set_header(http_client, line, trimmed_value);
        }
        line = eol ? eol + 2 : NULL;
    }
}

/** @brief Run one job; the result goes to the Lua task. */
static void run_job(job_t *job)
{
    msg_t *message = calloc(1, sizeof *message);
    if (!message) {
        return;
    }
    message->kind = MSG_RESP;
    message->id = job->id;
    message->status = -1;
    acc_t acc = {.hdr = {.max = HEAD_MAX}, .body = {.max = s_cfg.max_body}};
    esp_http_client_config_t client_config = {
        .url = job->url,
        .method = job->method,
        .timeout_ms = job->timeout_ms,
        .event_handler = on_client_event,
        .user_data = &acc,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t http_client = esp_http_client_init(&client_config);
    if (!http_client) {
        ESP_LOGW(TAG, "request %lu: bad URL %s", (unsigned long)job->id, job->url);
    } else {
        set_headers(http_client, job->headers);
        if (job->body_len) {
            esp_http_client_set_post_field(http_client, job->body, (int)job->body_len);
        }
        esp_err_t err = esp_http_client_perform(http_client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "request %lu: %s %s", (unsigned long)job->id, job->url, esp_err_to_name(err));
        } else if (acc.body.overflow || acc.hdr.overflow) {
            ESP_LOGW(TAG, "request %lu: response larger than %u bytes, dropped", (unsigned long)job->id,
                     (unsigned)s_cfg.max_body);
        } else {
            message->status = esp_http_client_get_status_code(http_client);
        }
        esp_http_client_cleanup(http_client);
    }
    if (message->status >= 0) {
        message->headers = acc.hdr.p;
        message->body = acc.body.p;
        message->body_len = acc.body.len;
        acc.hdr.p = acc.body.p = NULL;
    }
    free(acc.hdr.p);
    free(acc.body.p);
    if (is_aborted(job->id)) {
        msg_free(message);
        return;
    }
    post(message);
}

static void worker_task(void *arg)
{
    for (;;) {
        job_t *job;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!is_aborted(job->id)) {
            run_job(job);
        }
        job_free(job);
    }
}

// ---------------------------------------------------------------------------
// Incoming requests (HttpListener)
// ---------------------------------------------------------------------------
static void send_all(int socket_fd, const char *buffer, size_t remaining_bytes)
{
    while (remaining_bytes) {
        ssize_t bytes_sent = send(socket_fd, buffer, remaining_bytes, 0);
        if (bytes_sent <= 0) {
            return;
        }
        buffer += bytes_sent;
        remaining_bytes -= (size_t)bytes_sent;
    }
}

static void reply(int socket_fd, int status, const char *headers, const char *body, size_t body_len, bool head_only)
{
    char *response_head = malloc(HEAD_MAX + 128);
    if (!response_head) {
        return;
    }
    int formatted_head_length = http_format_head(response_head, HEAD_MAX + 128, status, headers, body_len);
    if (formatted_head_length < 0) {
        formatted_head_length = http_format_head(response_head, HEAD_MAX + 128, 500, NULL, 0);
        body_len = 0;
    }
    send_all(socket_fd, response_head, (size_t)formatted_head_length);
    free(response_head);
    if (!head_only && body_len) {
        send_all(socket_fd, body, body_len);
    }
}

/** @brief Read one request from @p s, hand it to Lua, send the answer. */
static void serve(int socket_fd, const char *peer)
{
    struct timeval socket_timeout = {.tv_sec = RECV_TIMEOUT};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof socket_timeout);
    char *buf = malloc(HEAD_MAX + 1);
    if (!buf) {
        return;
    }
    size_t len = 0;
    int head_len = 0;
    http_req_head_t request_head;
    while (len < HEAD_MAX) {
        ssize_t bytes_received = recv(socket_fd, buf + len, HEAD_MAX - len, 0);
        if (bytes_received <= 0) {
            free(buf);
            return; // closed / timeout before a full head
        }
        len += (size_t)bytes_received;
        char *copy = malloc(len + 1); // the parser writes terminators: keep buf intact while incomplete
        if (!copy) {
            free(buf);
            return;
        }
        memcpy(copy, buf, len);
        head_len = http_parse_head(copy, len, &request_head);
        if (head_len != 0) {
            free(buf);
            buf = copy;
            break;
        }
        free(copy);
    }
    if (head_len <= 0) {
        reply(socket_fd, head_len < 0 ? 400 : 431, NULL, NULL, 0, false);
        free(buf);
        return;
    }
    if (request_head.chunked) {
        reply(socket_fd, 411, NULL, NULL, 0, false);
        free(buf);
        return;
    }
    if (request_head.content_length > s_cfg.max_body) {
        reply(socket_fd, 413, NULL, NULL, 0, false);
        free(buf);
        return;
    }
    msg_t *message = calloc(1, sizeof *message);
    char *body = malloc(request_head.content_length + 1);
    if (!message || !body) {
        free(message);
        free(body);
        reply(socket_fd, 503, NULL, NULL, 0, false);
        free(buf);
        return;
    }
    size_t have = len - (size_t)head_len;
    if (have > request_head.content_length) {
        have = request_head.content_length; // pipelined data after the body is ignored (Connection: close)
    }
    memcpy(body, buf + head_len, have);
    while (have < request_head.content_length) {
        ssize_t bytes_received = recv(socket_fd, body + have, request_head.content_length - have, 0);
        if (bytes_received <= 0) {
            free(message);
            free(body);
            free(buf);
            return;
        }
        have += (size_t)bytes_received;
    }
    body[have] = 0;
    bool head_only = strcmp(request_head.method, "HEAD") == 0;
    message->kind = MSG_REQ;
    message->id = next_id();
    message->method = strdup(request_head.method);
    message->path = strdup(request_head.path);
    message->query = strdup(request_head.query);
    message->headers = strdup(request_head.headers);
    message->body = body;
    message->body_len = have;
    strlcpy(message->peer, peer, sizeof message->peer);
    free(buf);
    if (!message->method || !message->path || !message->query || !message->headers) {
        msg_free(message);
        reply(socket_fd, 503, NULL, NULL, 0, false);
        return;
    }
    uint32_t rid = message->id;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wait.rid = rid;
    s_wait.done = false;
    xSemaphoreGive(s_lock);
    xSemaphoreTake(s_wait_sem, 0); // drop a stale give
    if (!post(message)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_wait.rid = 0;
        xSemaphoreGive(s_lock);
        reply(socket_fd, 503, NULL, NULL, 0, false);
        return;
    }
    xSemaphoreTake(s_wait_sem, pdMS_TO_TICKS(s_cfg.listener_timeout_ms));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool done = s_wait.done;
    int status = s_wait.status;
    char *response_headers = s_wait.headers, *response_body = s_wait.body;
    size_t rbl = s_wait.body_len;
    s_wait.rid = 0;
    s_wait.done = false;
    s_wait.headers = s_wait.body = NULL;
    xSemaphoreGive(s_lock);
    if (done) {
        reply(socket_fd, status, response_headers, response_body, rbl, head_only);
    } else {
        ESP_LOGW(TAG, "request %lu from %s: no SendResponse within %d ms", (unsigned long)rid, peer,
                 s_cfg.listener_timeout_ms);
        reply(socket_fd, 504, NULL, NULL, 0, head_only);
    }
    free(response_headers);
    free(response_body);
}

static void server_task(void *arg)
{
    int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in listen_address = {.sin_family = AF_INET, .sin_port = htons(s_cfg.port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (listen_socket < 0 || bind(listen_socket, (struct sockaddr *)&listen_address, sizeof listen_address) != 0 || listen(listen_socket, 4) != 0) {
        ESP_LOGE(TAG, "listener: cannot bind TCP port %u", s_cfg.port);
        if (listen_socket >= 0) {
            close(listen_socket);
        }
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "HttpListener on TCP port %u", s_cfg.port);
    for (;;) {
        struct sockaddr_in peer_address;
        socklen_t peer_address_length = sizeof peer_address;
        int client_socket = accept(listen_socket, (struct sockaddr *)&peer_address, &peer_address_length);
        if (client_socket < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        char peer[16];
        inet_ntoa_r(peer_address.sin_addr, peer, sizeof peer);
        serve(client_socket, peer);
        shutdown(client_socket, SHUT_RDWR);
        close(client_socket);
    }
}

// ---------------------------------------------------------------------------
// Lua bindings (merged into __clu, used by http.lua only)
// ---------------------------------------------------------------------------
static const char *const METHODS[] = {
    [HTTP_METHOD_GET] = "GET",       [HTTP_METHOD_POST] = "POST",     [HTTP_METHOD_PUT] = "PUT",
    [HTTP_METHOD_PATCH] = "PATCH",   [HTTP_METHOD_DELETE] = "DELETE", [HTTP_METHOD_HEAD] = "HEAD",
    [HTTP_METHOD_OPTIONS] = "OPTIONS",
};

static char *dup_n(const char *source, size_t source_length)
{
    char *duplicated_string = malloc(source_length + 1);
    if (duplicated_string) {
        memcpy(duplicated_string, source, source_length);
        duplicated_string[source_length] = 0;
    }
    return duplicated_string;
}

// __clu.http_send(method, url, headers, body, timeout_s) -> id | nil, err
static int l_send(lua_State *L)
{
    const char *method = luaL_checkstring(L, 1);
    const char *url = luaL_checkstring(L, 2);
    const char *headers = luaL_optstring(L, 3, "");
    size_t blen = 0;
    const char *body = luaL_optlstring(L, 4, "", &blen);
    lua_Integer timeout = luaL_optinteger(L, 5, 5);
    int method_index = -1;
    for (int i = 0; i < (int)(sizeof METHODS / sizeof METHODS[0]); i++) {
        if (METHODS[i] && strcmp(METHODS[i], method) == 0) {
            method_index = i;
        }
    }
    if (method_index < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "unsupported method %s", method);
        return 2;
    }
    if (blen > s_cfg.max_body) {
        lua_pushnil(L);
        lua_pushstring(L, "request body too large");
        return 2;
    }
    if (!s_jobs) {
        lua_pushnil(L);
        lua_pushstring(L, "HTTP client not running");
        return 2;
    }
    job_t *job = calloc(1, sizeof *job);
    if (!job) {
        return luaL_error(L, "out of memory");
    }
    job->id = next_id();
    job->method = method_index;
    job->timeout_ms = (int)(timeout < 1 ? 1 : timeout > 255 ? 255 : timeout) * 1000;
    job->url = strdup(url);
    job->headers = strdup(headers);
    job->body = dup_n(body, blen);
    job->body_len = blen;
    if (!job->url || !job->headers || !job->body) {
        job_free(job);
        return luaL_error(L, "out of memory");
    }
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        job_free(job);
        lua_pushnil(L);
        lua_pushstring(L, "too many requests in progress");
        return 2;
    }
    lua_pushinteger(L, job->id);
    return 1;
}

// __clu.http_abort(id): drop the result (the request still runs if it already started)
static int l_abort(lua_State *L)
{
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_aborted[s_aborted_pos] = id;
    s_aborted_pos = (s_aborted_pos + 1) % ABORTED_MAX;
    xSemaphoreGive(s_lock);
    return 0;
}

/** @brief lua_pcall body of l_poll: push the ::msg_t (light userdata at 1). */
static int poll_push(lua_State *L)
{
    const msg_t *message = lua_touserdata(L, 1);
    if (message->kind == MSG_RESP) {
        lua_pushstring(L, "resp");
        lua_pushinteger(L, message->id);
        lua_pushinteger(L, message->status);
        lua_pushstring(L, message->headers ? message->headers : "");
        lua_pushlstring(L, message->body ? message->body : "", message->body_len);
        return 5;
    }
    lua_pushstring(L, "req");
    lua_pushinteger(L, message->id);
    lua_pushstring(L, message->method);
    lua_pushstring(L, message->path);
    lua_pushstring(L, message->query);
    lua_pushstring(L, message->headers);
    lua_pushlstring(L, message->body, message->body_len);
    lua_pushstring(L, message->peer);
    return 8;
}

// __clu.http_poll() -> "resp", id, status, headers, body | "req", id, method, path, query, headers, body, peer | nil
static int l_poll(lua_State *L)
{
    msg_t *message;
    if (!s_msgs || xQueueReceive(s_msgs, &message, 0) != pdTRUE) {
        return 0;
    }
    int top = lua_gettop(L);
    // Pushing can raise (out of memory): push under pcall so m is freed either way.
    lua_pushcfunction(L, poll_push);
    lua_pushlightuserdata(L, message);
    int call_status = lua_pcall(L, 1, LUA_MULTRET, 0);
    msg_free(message);
    if (call_status != LUA_OK) {
        return lua_error(L);
    }
    return lua_gettop(L) - top;
}

// __clu.http_respond(id, status, headers, body) -> true if the request was still waiting
static int l_respond(lua_State *L)
{
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    int status = (int)luaL_checkinteger(L, 2);
    const char *headers = luaL_optstring(L, 3, "");
    size_t blen = 0;
    const char *body = luaL_optlstring(L, 4, "", &blen);
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (id && s_wait.rid == id && !s_wait.done) {
        s_wait.status = status;
        s_wait.headers = strdup(headers);
        s_wait.body = dup_n(body, blen);
        s_wait.body_len = s_wait.body ? blen : 0;
        s_wait.done = true;
        ok = true;
        xSemaphoreGive(s_wait_sem);
    }
    xSemaphoreGive(s_lock);
    lua_pushboolean(L, ok);
    return 1;
}

// __clu.http_cfg() -> table
static int l_cfg(lua_State *L)
{
    lua_newtable(L);
    lua_pushinteger(L, s_cfg.port);
    lua_setfield(L, -2, "port");
    lua_pushinteger(L, (lua_Integer)s_cfg.max_body);
    lua_setfield(L, -2, "max_body");
    return 1;
}

static const luaL_Reg s_fns[] = {
    {"http_send", l_send}, {"http_abort", l_abort}, {"http_poll", l_poll},
    {"http_respond", l_respond}, {"http_cfg", l_cfg}, {NULL, NULL},
};

void clu_http_init(const clu_http_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_lock = xSemaphoreCreateMutex();
    s_wait_sem = xSemaphoreCreateBinary();
    s_msgs = xQueueCreate(MSG_QUEUE, sizeof(msg_t *));
    clu_lua_register_ext(s_fns, http_lua_start, http_lua_end - http_lua_start - 1, "=http");
}

void clu_http_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    s_jobs = xQueueCreate(JOB_QUEUE, sizeof(job_t *));
    xTaskCreate(worker_task, "http_req", WORKER_STACK, NULL, 4, NULL);
    xTaskCreate(server_task, "http_srv", SERVER_STACK, NULL, 4, NULL);
}
