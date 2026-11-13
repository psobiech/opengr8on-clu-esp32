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

static void msg_free(msg_t *m)
{
    if (!m) {
        return;
    }
    free(m->method);
    free(m->path);
    free(m->query);
    free(m->headers);
    free(m->body);
    free(m);
}

static void job_free(job_t *j)
{
    free(j->url);
    free(j->headers);
    free(j->body);
    free(j);
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
    bool r = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < ABORTED_MAX; i++) {
        r |= s_aborted[i] == id;
    }
    xSemaphoreGive(s_lock);
    return r;
}

/** @brief Hand @p m to the Lua task; freed if the queue stays full. */
static bool post(msg_t *m)
{
    if (xQueueSend(s_msgs, &m, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "queue to Lua full: %s %lu dropped", m->kind == MSG_REQ ? "request" : "response",
                 (unsigned long)m->id);
        msg_free(m);
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

static void buf_add(buf_t *b, const char *s, size_t n)
{
    if (b->overflow || b->len + n > b->max) {
        b->overflow = true;
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        char *p = realloc(b->p, cap);
        if (!p) {
            b->overflow = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

typedef struct {
    buf_t hdr, body;
} acc_t;

static esp_err_t on_client_event(esp_http_client_event_t *e)
{
    acc_t *a = e->user_data;
    switch (e->event_id) {
    case HTTP_EVENT_HEADERS_SENT: // (again) after a redirect: keep the final response only
        a->hdr.len = a->body.len = 0;
        a->hdr.overflow = a->body.overflow = false;
        break;
    case HTTP_EVENT_ON_HEADER:
        buf_add(&a->hdr, e->header_key, strlen(e->header_key));
        buf_add(&a->hdr, ": ", 2);
        buf_add(&a->hdr, e->header_value, strlen(e->header_value));
        buf_add(&a->hdr, "\r\n", 2);
        break;
    case HTTP_EVENT_ON_DATA: buf_add(&a->body, e->data, (size_t)e->data_len); break;
    default: break;
    }
    return ESP_OK;
}

/** @brief Apply "Name: value\r\n" lines to the client. */
static void set_headers(esp_http_client_handle_t c, char *h)
{
    for (char *line = h; line && *line;) {
        char *eol = strstr(line, "\r\n");
        if (eol) {
            *eol = 0;
        }
        char *colon = strchr(line, ':');
        if (colon && colon > line) {
            *colon = 0;
            char *v = colon + 1;
            while (*v == ' ') {
                v++;
            }
            esp_http_client_set_header(c, line, v);
        }
        line = eol ? eol + 2 : NULL;
    }
}

/** @brief Run one job; the result goes to the Lua task. */
static void run_job(job_t *j)
{
    msg_t *m = calloc(1, sizeof *m);
    if (!m) {
        return;
    }
    m->kind = MSG_RESP;
    m->id = j->id;
    m->status = -1;
    acc_t acc = {.hdr = {.max = HEAD_MAX}, .body = {.max = s_cfg.max_body}};
    esp_http_client_config_t cc = {
        .url = j->url,
        .method = j->method,
        .timeout_ms = j->timeout_ms,
        .event_handler = on_client_event,
        .user_data = &acc,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cc);
    if (!c) {
        ESP_LOGW(TAG, "request %lu: bad URL %s", (unsigned long)j->id, j->url);
    } else {
        set_headers(c, j->headers);
        if (j->body_len) {
            esp_http_client_set_post_field(c, j->body, (int)j->body_len);
        }
        esp_err_t err = esp_http_client_perform(c);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "request %lu: %s %s", (unsigned long)j->id, j->url, esp_err_to_name(err));
        } else if (acc.body.overflow || acc.hdr.overflow) {
            ESP_LOGW(TAG, "request %lu: response larger than %u bytes, dropped", (unsigned long)j->id,
                     (unsigned)s_cfg.max_body);
        } else {
            m->status = esp_http_client_get_status_code(c);
        }
        esp_http_client_cleanup(c);
    }
    if (m->status >= 0) {
        m->headers = acc.hdr.p;
        m->body = acc.body.p;
        m->body_len = acc.body.len;
        acc.hdr.p = acc.body.p = NULL;
    }
    free(acc.hdr.p);
    free(acc.body.p);
    if (is_aborted(j->id)) {
        msg_free(m);
        return;
    }
    post(m);
}

static void worker_task(void *arg)
{
    for (;;) {
        job_t *j;
        if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!is_aborted(j->id)) {
            run_job(j);
        }
        job_free(j);
    }
}

// ---------------------------------------------------------------------------
// Incoming requests (HttpListener)
// ---------------------------------------------------------------------------
static void send_all(int s, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(s, p, n, 0);
        if (w <= 0) {
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

static void reply(int s, int status, const char *headers, const char *body, size_t body_len, bool head_only)
{
    char *h = malloc(HEAD_MAX + 128);
    if (!h) {
        return;
    }
    int n = http_format_head(h, HEAD_MAX + 128, status, headers, body_len);
    if (n < 0) {
        n = http_format_head(h, HEAD_MAX + 128, 500, NULL, 0);
        body_len = 0;
    }
    send_all(s, h, (size_t)n);
    free(h);
    if (!head_only && body_len) {
        send_all(s, body, body_len);
    }
}

/** @brief Read one request from @p s, hand it to Lua, send the answer. */
static void serve(int s, const char *peer)
{
    struct timeval tv = {.tv_sec = RECV_TIMEOUT};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char *buf = malloc(HEAD_MAX + 1);
    if (!buf) {
        return;
    }
    size_t len = 0;
    int head_len = 0;
    http_req_head_t h;
    while (len < HEAD_MAX) {
        ssize_t r = recv(s, buf + len, HEAD_MAX - len, 0);
        if (r <= 0) {
            free(buf);
            return; // closed / timeout before a full head
        }
        len += (size_t)r;
        char *copy = malloc(len + 1); // the parser writes terminators: keep buf intact while incomplete
        if (!copy) {
            free(buf);
            return;
        }
        memcpy(copy, buf, len);
        head_len = http_parse_head(copy, len, &h);
        if (head_len != 0) {
            free(buf);
            buf = copy;
            break;
        }
        free(copy);
    }
    if (head_len <= 0) {
        reply(s, head_len < 0 ? 400 : 431, NULL, NULL, 0, false);
        free(buf);
        return;
    }
    if (h.chunked) {
        reply(s, 411, NULL, NULL, 0, false);
        free(buf);
        return;
    }
    if (h.content_length > s_cfg.max_body) {
        reply(s, 413, NULL, NULL, 0, false);
        free(buf);
        return;
    }
    msg_t *m = calloc(1, sizeof *m);
    char *body = malloc(h.content_length + 1);
    if (!m || !body) {
        free(m);
        free(body);
        reply(s, 503, NULL, NULL, 0, false);
        free(buf);
        return;
    }
    size_t have = len - (size_t)head_len;
    if (have > h.content_length) {
        have = h.content_length; // pipelined data after the body is ignored (Connection: close)
    }
    memcpy(body, buf + head_len, have);
    while (have < h.content_length) {
        ssize_t r = recv(s, body + have, h.content_length - have, 0);
        if (r <= 0) {
            free(m);
            free(body);
            free(buf);
            return;
        }
        have += (size_t)r;
    }
    body[have] = 0;
    bool head_only = strcmp(h.method, "HEAD") == 0;
    m->kind = MSG_REQ;
    m->id = next_id();
    m->method = strdup(h.method);
    m->path = strdup(h.path);
    m->query = strdup(h.query);
    m->headers = strdup(h.headers);
    m->body = body;
    m->body_len = have;
    strlcpy(m->peer, peer, sizeof m->peer);
    free(buf);
    if (!m->method || !m->path || !m->query || !m->headers) {
        msg_free(m);
        reply(s, 503, NULL, NULL, 0, false);
        return;
    }
    uint32_t rid = m->id;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wait.rid = rid;
    s_wait.done = false;
    xSemaphoreGive(s_lock);
    xSemaphoreTake(s_wait_sem, 0); // drop a stale give
    if (!post(m)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_wait.rid = 0;
        xSemaphoreGive(s_lock);
        reply(s, 503, NULL, NULL, 0, false);
        return;
    }
    xSemaphoreTake(s_wait_sem, pdMS_TO_TICKS(s_cfg.listener_timeout_ms));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool done = s_wait.done;
    int status = s_wait.status;
    char *rh = s_wait.headers, *rb = s_wait.body;
    size_t rbl = s_wait.body_len;
    s_wait.rid = 0;
    s_wait.done = false;
    s_wait.headers = s_wait.body = NULL;
    xSemaphoreGive(s_lock);
    if (done) {
        reply(s, status, rh, rb, rbl, head_only);
    } else {
        ESP_LOGW(TAG, "request %lu from %s: no SendResponse within %d ms", (unsigned long)rid, peer,
                 s_cfg.listener_timeout_ms);
        reply(s, 504, NULL, NULL, 0, head_only);
    }
    free(rh);
    free(rb);
}

static void server_task(void *arg)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(s_cfg.port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 4) != 0) {
        ESP_LOGE(TAG, "listener: cannot bind TCP port %u", s_cfg.port);
        if (ls >= 0) {
            close(ls);
        }
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "HttpListener on TCP port %u", s_cfg.port);
    for (;;) {
        struct sockaddr_in pa;
        socklen_t pl = sizeof pa;
        int c = accept(ls, (struct sockaddr *)&pa, &pl);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        char peer[16];
        inet_ntoa_r(pa.sin_addr, peer, sizeof peer);
        serve(c, peer);
        shutdown(c, SHUT_RDWR);
        close(c);
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

static char *dup_n(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
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
    int m = -1;
    for (int i = 0; i < (int)(sizeof METHODS / sizeof METHODS[0]); i++) {
        if (METHODS[i] && strcmp(METHODS[i], method) == 0) {
            m = i;
        }
    }
    if (m < 0) {
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
    job_t *j = calloc(1, sizeof *j);
    if (!j) {
        return luaL_error(L, "out of memory");
    }
    j->id = next_id();
    j->method = m;
    j->timeout_ms = (int)(timeout < 1 ? 1 : timeout > 255 ? 255 : timeout) * 1000;
    j->url = strdup(url);
    j->headers = strdup(headers);
    j->body = dup_n(body, blen);
    j->body_len = blen;
    if (!j->url || !j->headers || !j->body) {
        job_free(j);
        return luaL_error(L, "out of memory");
    }
    if (xQueueSend(s_jobs, &j, 0) != pdTRUE) {
        job_free(j);
        lua_pushnil(L);
        lua_pushstring(L, "too many requests in progress");
        return 2;
    }
    lua_pushinteger(L, j->id);
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
    const msg_t *m = lua_touserdata(L, 1);
    if (m->kind == MSG_RESP) {
        lua_pushstring(L, "resp");
        lua_pushinteger(L, m->id);
        lua_pushinteger(L, m->status);
        lua_pushstring(L, m->headers ? m->headers : "");
        lua_pushlstring(L, m->body ? m->body : "", m->body_len);
        return 5;
    }
    lua_pushstring(L, "req");
    lua_pushinteger(L, m->id);
    lua_pushstring(L, m->method);
    lua_pushstring(L, m->path);
    lua_pushstring(L, m->query);
    lua_pushstring(L, m->headers);
    lua_pushlstring(L, m->body, m->body_len);
    lua_pushstring(L, m->peer);
    return 8;
}

// __clu.http_poll() -> "resp", id, status, headers, body | "req", id, method, path, query, headers, body, peer | nil
static int l_poll(lua_State *L)
{
    msg_t *m;
    if (!s_msgs || xQueueReceive(s_msgs, &m, 0) != pdTRUE) {
        return 0;
    }
    int top = lua_gettop(L);
    // Pushing can raise (out of memory): push under pcall so m is freed either way.
    lua_pushcfunction(L, poll_push);
    lua_pushlightuserdata(L, m);
    int st = lua_pcall(L, 1, LUA_MULTRET, 0);
    msg_free(m);
    if (st != LUA_OK) {
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
