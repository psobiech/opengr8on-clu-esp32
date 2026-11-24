// Host tests for the GATE HTTP objects, C side (clu_http.c, included: state is static): the Lua
// bindings in a real VM, outgoing requests run by the worker against a fake esp_http_client,
// incoming requests served against fake sockets (the Lua side answers from inside the queue send,
// as the Lua task would while the server waits) and the listener task. http_proto.c is linked.
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lualib.h"

#include "../../components/clu_http/clu_http.c"

// The firmware embeds http.lua via EMBED_TXTFILES (NUL-terminated); test_http.lua tests it.
__asm__(".pushsection .rodata\n"
        ".global _binary_http_lua_start\n_binary_http_lua_start:\n.asciz \"return 1\"\n"
        ".global _binary_http_lua_end\n_binary_http_lua_end:\n"
        ".popsection\n");

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- fakes: state
#define FAKE_QUEUE_CAPACITY 16
#define CLIENT_SOCKET       7
#define LISTEN_SOCKET       3

struct host_queue {
    void *items[FAKE_QUEUE_CAPACITY];
    UBaseType_t capacity;
    int count, head;
};
static struct host_queue s_queue_pool[4];
static int s_queues_created;
static bool s_message_queue_full;
static bool s_job_receive_fails_once;
static const char *s_responder_code; // Lua run when the server hands a request to the Lua task

static jmp_buf s_task_exit;
static int s_tasks_created;
static const char *s_task_names[4];
static uint32_t s_task_stacks[4];
static int s_task_deletes, s_task_delays;

static const luaL_Reg *s_registered_functions;
static size_t s_registered_source_length;
static const char *s_registered_chunk_name;
static lua_State *s_lua;

static struct esp_http_client { int unused; } s_fake_client;
static esp_http_client_config_t s_last_client_config;
static char s_last_client_url[128]; // copy: the worker frees the job's URL after the request
static bool s_client_init_fails, s_redirect_first;
static esp_err_t s_perform_result;
static int s_status_code;
static const char *s_response_header_name, *s_response_header_value;
static const char *s_response_body;
static char s_client_headers_set[256];
static char s_post_field[128];
static int s_post_field_length = -1;
static int s_client_inits, s_client_cleanups;

static const char *s_receive_chunks[4];
static size_t s_receive_chunk_lengths[4];
static int s_receive_chunk_count, s_receive_chunk_index;
static size_t s_receive_chunk_offset;
static char s_sent[16384];
static size_t s_sent_length;
static size_t s_send_limit = 10; // bytes per send() call: send_all() loops
static bool s_send_fails;
static int s_receive_timeout_s;

static int s_socket_result = LISTEN_SOCKET, s_bind_result;
static int s_bound_port;
static const int *s_accept_results;
static int s_accept_result_count, s_accept_index;
static int s_last_closed_socket = -1, s_last_shut_down_socket = -1, s_closes;

static void test_bindings_before_start(void);
static void test_outgoing_requests(void);
static void test_incoming_requests(void);
static void test_listener_task(void);
static void lua_run(const char *code);
static bool global_is(const char *name, const char *expected);
static lua_Integer global_integer(const char *name);
static bool global_is_nil(const char *name);
static void run_worker(void);
static void serve_request(const char *first_chunk, const char *second_chunk);
static void serve_raw(const char *chunk, size_t chunk_length);
static bool sent_starts_with(const char *prefix);
static bool sent_ends_with(const char *suffix);
static void client_event(esp_http_client_event_id_t event_id, const char *header_name, const char *header_value,
                         const char *data);

int main(void)
{
    s_lua = luaL_newstate();
    luaL_openlibs(s_lua);
    for (const luaL_Reg *function = s_fns; function->name; function++) {
        lua_register(s_lua, function->name, function->func);
    }
    test_bindings_before_start();
    test_outgoing_requests();
    test_incoming_requests();
    test_listener_task();
    lua_close(s_lua);
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- init, start, config
static void test_bindings_before_start(void)
{
    lua_run("result = http_poll()");
    check("poll before init: nothing", global_is_nil("result"));
    lua_run("id, err = http_send('GET', 'http://host/')");
    check("send before start: client not running", global_is_nil("id") && global_is("err", "HTTP client not running"));

    const clu_http_cfg_t config = {.port = 8080, .max_body = 64, .listener_timeout_ms = 500};
    clu_http_init(&config);
    check("bindings registered with http.lua", s_registered_functions == s_fns &&
                                                   strcmp(s_registered_chunk_name, "=http") == 0 &&
                                                   s_registered_source_length == strlen("return 1"));
    lua_run("config = http_cfg(); port, max_body = config.port, config.max_body");
    check("http_cfg: port and body limit", global_integer("port") == 8080 && global_integer("max_body") == 64);
    lua_run("result = http_poll()");
    check("poll with nothing queued: nothing", global_is_nil("result"));

    clu_http_start();
    check("start: worker and server tasks", s_tasks_created == 2 && strcmp(s_task_names[0], "http_req") == 0 &&
                                               s_task_stacks[0] == 10 * 1024 && strcmp(s_task_names[1], "http_srv") == 0 &&
                                               s_task_stacks[1] == 4 * 1024);
    clu_http_start();
    check("second start does nothing", s_tasks_created == 2);
}

// ---------------------------------------------------------------- outgoing requests
static void test_outgoing_requests(void)
{
    lua_run("id, err = http_send('BREW', 'http://host/')");
    check("unsupported method refused", global_is_nil("id") && global_is("err", "unsupported method BREW"));
    lua_run("id, err = http_send('POST', 'http://host/', '', string.rep('x', 65))");
    check("body over max_body refused", global_is_nil("id") && global_is("err", "request body too large"));

    // POST with headers (lines without a name are skipped) and a body; redirect, then the answer.
    lua_run("id = http_send('POST', 'http://host/p', 'A: b\\r\\nNoColon\\r\\n: empty\\r\\nC:   d', 'xyz', 0)");
    const lua_Integer post_id = global_integer("id");
    check("request queued, id returned", post_id > 0);
    s_status_code = 200;
    s_response_header_name = "K";
    s_response_header_value = "v";
    s_response_body = "hello world";
    s_redirect_first = true;
    run_worker();
    check("client: URL, method, keep-alive off, certificate bundle",
          strcmp(s_last_client_config.url, "http://host/p") == 0 && s_last_client_config.method == HTTP_METHOD_POST &&
              !s_last_client_config.keep_alive_enable && s_last_client_config.crt_bundle_attach == esp_crt_bundle_attach);
    check("timeout below 1 s raised to 1 s", s_last_client_config.timeout_ms == 1000);
    check("named header lines set, value trimmed", strcmp(s_client_headers_set, "A=b;C=d;") == 0);
    check("body sent as post field", s_post_field_length == 3 && strcmp(s_post_field, "xyz") == 0);
    check("client cleaned up", s_client_cleanups == 1);
    lua_run("kind, rid, status, headers, body = http_poll()");
    check("response for the request, redirect dropped", global_is("kind", "resp") && global_integer("rid") == post_id &&
                                                            global_integer("status") == 200 &&
                                                            global_is("headers", "K: v\r\n") &&
                                                            global_is("body", "hello world"));
    s_redirect_first = false;

    s_client_headers_set[0] = '\0';
    s_post_field_length = -1;
    s_client_init_fails = true;
    lua_run("id = http_send('GET', 'bad url', nil, nil, 999)");
    run_worker();
    check("timeout above 255 s lowered", s_last_client_config.timeout_ms == 255000);
    check("GET without body: no post field", s_post_field_length == -1);
    lua_run("kind, rid, status, headers, body = http_poll()");
    check("bad URL: status -1, empty headers and body", global_is("kind", "resp") && global_integer("status") == -1 &&
                                                            global_is("headers", "") && global_is("body", ""));
    s_client_init_fails = false;

    s_perform_result = ESP_ERR_TIMEOUT;
    lua_run("id = http_send('DELETE', 'http://host/')");
    run_worker();
    lua_run("kind, rid, status = http_poll()");
    check("connection / timeout error: status -1", global_integer("status") == -1);
    check("default timeout 5 s", s_last_client_config.timeout_ms == 5000 &&
                                     s_last_client_config.method == HTTP_METHOD_DELETE);
    s_perform_result = ESP_OK;

    s_response_body = "0123456789012345678901234567890123456789012345678901234567890123456789"; // 70 > 64
    lua_run("id = http_send('OPTIONS', 'http://host/')");
    run_worker();
    lua_run("kind, rid, status = http_poll()");
    check("response over max_body: status -1", global_integer("status") == -1 &&
                                                   s_last_client_config.method == HTTP_METHOD_OPTIONS);
    s_response_body = "ok";

    const int inits_before_abort = s_client_inits;
    lua_run("id = http_send('GET', 'http://host/'); http_abort(id)");
    run_worker();
    lua_run("result = http_poll()");
    check("aborted before it ran: not run, no result", s_client_inits == inits_before_abort && global_is_nil("result"));

    // Aborted while running: run, result dropped.
    lua_run("http_abort(4242)");
    job_t *running_job = calloc(1, sizeof *running_job);
    *running_job = (job_t){.id = 4242, .url = strdup("http://host/"), .headers = strdup(""), .body = strdup("")};
    run_job(running_job);
    job_free(running_job);
    lua_run("result = http_poll()");
    check("aborted while running: result dropped", s_client_inits == inits_before_abort + 1 && global_is_nil("result"));

    s_message_queue_full = true;
    lua_run("id = http_send('PUT', 'http://host/')");
    run_worker();
    s_message_queue_full = false;
    lua_run("result = http_poll()");
    check("queue to Lua full: result dropped", global_is_nil("result"));

    lua_run("ids = {}; for i = 1, 5 do ids[i], err = http_send('GET', 'http://host/') end");
    check("job queue full: too many requests", global_is("err", "too many requests in progress"));
    lua_run("for i = 1, 4 do http_abort(ids[i]) end");
    s_job_receive_fails_once = true;
    run_worker();
    lua_run("result = http_poll()");
    check("queued jobs aborted: worker drops them", global_is_nil("result"));

    s_client_headers_set[0] = '\0';
    lua_run("id = http_send('HEAD', 'http://host/', 'X: y', 'b', 3)");
    run_worker();
    lua_run("kind, rid, status = http_poll()");
    check("last header line without CRLF, timeout in seconds", strcmp(s_client_headers_set, "X=y;") == 0 &&
                                                                   s_last_client_config.method == HTTP_METHOD_HEAD &&
                                                                   s_last_client_config.timeout_ms == 3000 &&
                                                                   global_integer("status") == 200);
}

// ---------------------------------------------------------------- incoming requests
static void test_incoming_requests(void)
{
    s_responder_code = "kind, rid, method, path, query, headers, body, peer = http_poll();"
                       "ok = http_respond(rid, 201, 'X-A: 1\\r\\n', 'hello')";
    serve_request("GET /api/x?a=1 HTTP/1.1\r\nHost: h\r\n", "\r\n");
    check("request handed to Lua", global_is("kind", "req") && global_is("method", "GET") &&
                                       global_is("path", "/api/x") && global_is("query", "a=1") &&
                                       global_is("headers", "Host: h\r\n") && global_is("body", "") &&
                                       global_is("peer", "10.0.0.2") && global_is("ok", "true"));
    check("response: status line, headers, body", sent_starts_with("HTTP/1.1 201 ") &&
                                                      strstr(s_sent, "X-A: 1\r\nContent-Length: 5\r\n") &&
                                                      sent_ends_with("\r\n\r\nhello"));
    check("idle connection times out after 5 s", s_receive_timeout_s == 5);

    serve_request("POST /p HTTP/1.1\r\nContent-Length: 6\r\n\r\nabc", "defEXTRA");
    check("body completed from the next receive, extra bytes ignored", global_is("body", "abcdef") &&
                                                                           global_is("method", "POST"));
    serve_request("POST /p HTTP/1.1\r\nContent-Length: 2\r\n\r\nabcd", NULL);
    check("pipelined data after the body ignored", global_is("body", "ab"));

    serve_request("HEAD / HTTP/1.1\r\n\r\n", NULL);
    check("HEAD: head with the body length, no body", sent_starts_with("HTTP/1.1 201 ") &&
                                                          strstr(s_sent, "Content-Length: 5\r\n") &&
                                                          sent_ends_with("\r\n\r\n"));

    s_responder_code = "kind, rid = http_poll(); wrong = http_respond(rid + 1000, 200);"
                       "zero = http_respond(0, 200); first = http_respond(rid, 200, nil, 'a');"
                       "again = http_respond(rid, 500)";
    serve_request("GET / HTTP/1.1\r\n\r\n", NULL);
    check("respond: only to the waiting request, only once", global_is("wrong", "false") && global_is("zero", "false") &&
                                                                 global_is("first", "true") && global_is("again", "false"));
    check("first response sent", sent_starts_with("HTTP/1.1 200 ") && sent_ends_with("\r\n\r\na"));

    s_responder_code = "kind, rid = http_poll(); http_respond(rid, 200, string.rep('H', 5000), 'body')";
    serve_request("GET / HTTP/1.1\r\n\r\n", NULL);
    check("response head too large: 500 without body", sent_starts_with("HTTP/1.1 500 ") &&
                                                           strstr(s_sent, "Content-Length: 0\r\n") &&
                                                           sent_ends_with("\r\n\r\n"));

    s_responder_code = NULL;
    serve_request("GET /late HTTP/1.1\r\n\r\n", NULL);
    check("no SendResponse in time: 504", sent_starts_with("HTTP/1.1 504 "));
    lua_run("kind, rid, method, path = http_poll(); late = http_respond(rid, 200)");
    check("late response refused", global_is("path", "/late") && global_is("late", "false"));

    s_message_queue_full = true;
    serve_request("GET / HTTP/1.1\r\n\r\n", NULL);
    s_message_queue_full = false;
    check("queue to Lua full: 503", sent_starts_with("HTTP/1.1 503 "));

    serve_request("GET / HT", NULL);
    check("connection closed before a full head: no answer", s_sent_length == 0);
    serve_request("garbage\r\n\r\n", NULL);
    check("malformed head: 400", sent_starts_with("HTTP/1.1 400 "));
    static char oversized_head[HEAD_MAX];
    memset(oversized_head, 'a', sizeof oversized_head);
    serve_raw(oversized_head, sizeof oversized_head);
    check("head over 4 KiB: 431", sent_starts_with("HTTP/1.1 431 "));
    serve_request("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", NULL);
    check("chunked body: 411", sent_starts_with("HTTP/1.1 411 "));
    serve_request("POST / HTTP/1.1\r\nContent-Length: 65\r\n\r\n", NULL);
    check("body over max_body: 413", sent_starts_with("HTTP/1.1 413 "));
    serve_request("POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nab", NULL);
    check("connection closed inside the body: no answer", s_sent_length == 0);
    lua_run("result = http_poll()");
    check("nothing left for Lua", global_is_nil("result"));

    s_responder_code = "kind, rid = http_poll(); http_respond(rid, 200, nil, 'body')";
    s_send_fails = true;
    serve_request("GET / HTTP/1.1\r\n\r\n", NULL);
    s_send_fails = false;
    check("peer gone while sending: nothing more sent", s_sent_length == 0);
}

// ---------------------------------------------------------------- listener task
static void test_listener_task(void)
{
    s_socket_result = -1;
    server_task(NULL);
    check("no socket: task deleted", s_task_deletes == 1 && s_closes == 0);

    s_socket_result = LISTEN_SOCKET;
    s_bind_result = -1;
    server_task(NULL);
    check("port taken: socket closed, task deleted", s_task_deletes == 2 && s_last_closed_socket == LISTEN_SOCKET);
    check("bound to the configured port", s_bound_port == 8080);

    s_bind_result = 0;
    const int accept_results[] = {-1, CLIENT_SOCKET};
    s_accept_results = accept_results;
    s_accept_result_count = 2;
    s_accept_index = 0;
    s_receive_chunks[0] = "GET /served HTTP/1.1\r\n\r\n";
    s_receive_chunk_lengths[0] = strlen(s_receive_chunks[0]);
    s_receive_chunk_count = 1;
    s_receive_chunk_index = 0;
    s_receive_chunk_offset = 0;
    s_sent_length = 0;
    s_sent[0] = '\0';
    s_responder_code = "kind, rid, method, path, query, headers, body, peer = http_poll(); http_respond(rid, 204)";
    if (setjmp(s_task_exit) == 0) {
        server_task(NULL);
    }
    check("failed accept: retried after a delay", s_task_delays == 1);
    check("client served with its address", global_is("path", "/served") && global_is("peer", "192.168.1.20") &&
                                                sent_starts_with("HTTP/1.1 204 "));
    check("client connection shut down and closed", s_last_shut_down_socket == CLIENT_SOCKET &&
                                                        s_last_closed_socket == CLIENT_SOCKET);
    s_responder_code = NULL;
}

// ---------------------------------------------------------------- helpers
static void lua_run(const char *code)
{
    if (luaL_dostring(s_lua, code) != LUA_OK) {
        printf("FAIL lua: %s\n", lua_tostring(s_lua, -1));
        failures++;
        lua_pop(s_lua, 1);
    }
}

/** @brief Global @p name as a string (numbers and booleans converted) equals @p expected. */
static bool global_is(const char *name, const char *expected)
{
    lua_getglobal(s_lua, name);
    const char *value = luaL_tolstring(s_lua, -1, NULL);
    bool equal = value && strcmp(value, expected) == 0;
    lua_pop(s_lua, 2);
    return equal;
}

static lua_Integer global_integer(const char *name)
{
    lua_getglobal(s_lua, name);
    lua_Integer value = lua_tointeger(s_lua, -1);
    lua_pop(s_lua, 1);
    return value;
}

static bool global_is_nil(const char *name)
{
    lua_getglobal(s_lua, name);
    bool is_nil = lua_isnil(s_lua, -1);
    lua_pop(s_lua, 1);
    return is_nil;
}

/** @brief Run the worker until the job queue is empty. */
static void run_worker(void)
{
    if (setjmp(s_task_exit) == 0) {
        worker_task(NULL);
    }
}

/** @brief Serve one connection that receives @p first_chunk, then @p second_chunk (NULL: none). */
static void serve_request(const char *first_chunk, const char *second_chunk)
{
    s_receive_chunks[0] = first_chunk;
    s_receive_chunk_lengths[0] = strlen(first_chunk);
    s_receive_chunks[1] = second_chunk;
    s_receive_chunk_lengths[1] = second_chunk ? strlen(second_chunk) : 0;
    s_receive_chunk_count = second_chunk ? 2 : 1;
    s_receive_chunk_index = 0;
    s_receive_chunk_offset = 0;
    s_sent_length = 0;
    s_sent[0] = '\0';
    lua_run("kind, rid, method, path, query, headers, body, peer, ok = nil");
    serve(CLIENT_SOCKET, "10.0.0.2");
}

static void serve_raw(const char *chunk, size_t chunk_length)
{
    s_receive_chunks[0] = chunk;
    s_receive_chunk_lengths[0] = chunk_length;
    s_receive_chunk_count = 1;
    s_receive_chunk_index = 0;
    s_receive_chunk_offset = 0;
    s_sent_length = 0;
    s_sent[0] = '\0';
    serve(CLIENT_SOCKET, "10.0.0.2");
}

static bool sent_starts_with(const char *prefix)
{
    return strncmp(s_sent, prefix, strlen(prefix)) == 0;
}

static bool sent_ends_with(const char *suffix)
{
    size_t suffix_length = strlen(suffix);
    return s_sent_length >= suffix_length && memcmp(s_sent + s_sent_length - suffix_length, suffix, suffix_length) == 0;
}

// ---------------------------------------------------------------- fakes: Lua runtime
void clu_lua_register_ext(const luaL_Reg *fns, const char *src, size_t len, const char *name)
{
    s_registered_functions = fns;
    s_registered_source_length = len;
    s_registered_chunk_name = name;
}

// ---------------------------------------------------------------- fakes: FreeRTOS
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item)
{
    struct host_queue *queue = &s_queue_pool[s_queues_created++];
    queue->capacity = len < FAKE_QUEUE_CAPACITY ? len : FAKE_QUEUE_CAPACITY;
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    if ((q == s_msgs && s_message_queue_full) || (UBaseType_t)q->count == q->capacity) {
        return pdFALSE;
    }
    memcpy(&q->items[(q->head + q->count) % FAKE_QUEUE_CAPACITY], item, sizeof(void *));
    q->count++;
    if (q == s_msgs && s_responder_code) {
        lua_run(s_responder_code); // the Lua task picks the request up while the server waits
    }
    return pdTRUE;
}

/** @brief Empty job queue with an endless wait: leave the worker task. */
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    if (q == s_jobs && s_job_receive_fails_once) {
        s_job_receive_fails_once = false;
        return pdFALSE;
    }
    if (q->count == 0) {
        if (wait == portMAX_DELAY) {
            longjmp(s_task_exit, 1);
        }
        return pdFALSE;
    }
    memcpy(item, &q->items[q->head], sizeof(void *));
    q->head = (q->head + 1) % FAKE_QUEUE_CAPACITY;
    q->count--;
    return pdTRUE;
}

BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *h)
{
    s_task_names[s_tasks_created] = name;
    s_task_stacks[s_tasks_created] = stack;
    s_tasks_created++;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t t)
{
    s_task_deletes++;
}

void vTaskDelay(TickType_t t)
{
    s_task_delays++;
}

// ---------------------------------------------------------------- fakes: HTTP client
esp_err_t esp_crt_bundle_attach(void *conf)
{
    return ESP_OK;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    s_client_inits++;
    s_last_client_config = *config;
    snprintf(s_last_client_url, sizeof s_last_client_url, "%s", config->url);
    s_last_client_config.url = s_last_client_url;
    return s_client_init_fails ? NULL : &s_fake_client;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    size_t used = strlen(s_client_headers_set);
    snprintf(s_client_headers_set + used, sizeof s_client_headers_set - used, "%s=%s;", key, value);
    return ESP_OK;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len)
{
    s_post_field_length = len;
    snprintf(s_post_field, sizeof s_post_field, "%.*s", len, data);
    return ESP_OK;
}

/** @brief Events of one exchange: optional redirect (its headers and body dropped), header, body in two parts. */
esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    client_event(HTTP_EVENT_ON_CONNECTED, NULL, NULL, NULL);
    client_event(HTTP_EVENT_HEADERS_SENT, NULL, NULL, NULL);
    if (s_redirect_first) {
        client_event(HTTP_EVENT_ON_HEADER, "Location", "http://host/q", NULL);
        client_event(HTTP_EVENT_ON_DATA, NULL, NULL, "moved");
        client_event(HTTP_EVENT_HEADERS_SENT, NULL, NULL, NULL);
    }
    if (s_response_header_name) {
        client_event(HTTP_EVENT_ON_HEADER, s_response_header_name, s_response_header_value, NULL);
    }
    if (s_response_body) {
        size_t body_length = strlen(s_response_body);
        char first_part[128];
        snprintf(first_part, sizeof first_part, "%.*s", (int)(body_length / 2), s_response_body);
        client_event(HTTP_EVENT_ON_DATA, NULL, NULL, first_part);
        client_event(HTTP_EVENT_ON_DATA, NULL, NULL, s_response_body + body_length / 2);
    }
    client_event(HTTP_EVENT_ON_FINISH, NULL, NULL, NULL);
    return s_perform_result;
}

static void client_event(esp_http_client_event_id_t event_id, const char *header_name, const char *header_value,
                         const char *data)
{
    esp_http_client_event_t event = {
        .event_id = event_id,
        .client = &s_fake_client,
        .user_data = s_last_client_config.user_data,
        .header_key = (char *)header_name,
        .header_value = (char *)header_value,
        .data = (void *)data,
        .data_len = data ? (int)strlen(data) : 0,
    };
    s_last_client_config.event_handler(&event);
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    return s_status_code;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    s_client_cleanups++;
    return ESP_OK;
}

// ---------------------------------------------------------------- fakes: sockets
int host_socket(int domain, int type, int proto)
{
    return s_socket_result;
}

int host_bind(int s, const struct sockaddr *a, socklen_t len)
{
    s_bound_port = ntohs(((const struct sockaddr_in *)a)->sin_port);
    return s_bind_result;
}

int host_listen(int s, int backlog)
{
    return 0;
}

int host_setsockopt(int s, int level, int opt, const void *v, socklen_t len)
{
    if (opt == SO_RCVTIMEO) {
        s_receive_timeout_s = (int)((const struct timeval *)v)->tv_sec;
    }
    return 0;
}

/** @brief Next scripted accept result; when they ran out, leave the listener task. */
int host_accept(int s, struct sockaddr *a, socklen_t *al)
{
    if (s_accept_index == s_accept_result_count) {
        longjmp(s_task_exit, 1);
    }
    struct sockaddr_in *peer_address = (struct sockaddr_in *)a;
    peer_address->sin_family = AF_INET;
    peer_address->sin_addr.s_addr = inet_addr("192.168.1.20");
    return s_accept_results[s_accept_index++];
}

/** @brief The scripted chunks in order, each cut to @p len; 0 (closed) when they ran out. */
ssize_t host_recv(int s, void *buf, size_t len, int flags)
{
    if (s_receive_chunk_index == s_receive_chunk_count) {
        return 0;
    }
    size_t chunk_left = s_receive_chunk_lengths[s_receive_chunk_index] - s_receive_chunk_offset;
    size_t copied = chunk_left < len ? chunk_left : len;
    memcpy(buf, s_receive_chunks[s_receive_chunk_index] + s_receive_chunk_offset, copied);
    s_receive_chunk_offset += copied;
    if (s_receive_chunk_offset == s_receive_chunk_lengths[s_receive_chunk_index]) {
        s_receive_chunk_index++;
        s_receive_chunk_offset = 0;
    }
    return (ssize_t)copied;
}

ssize_t host_send(int s, const void *buf, size_t len, int flags)
{
    if (s_send_fails) {
        return -1;
    }
    size_t accepted = len < s_send_limit ? len : s_send_limit;
    if (s_sent_length + accepted >= sizeof s_sent) {
        return 0;
    }
    memcpy(s_sent + s_sent_length, buf, accepted);
    s_sent_length += accepted;
    s_sent[s_sent_length] = '\0';
    return (ssize_t)accepted;
}

int host_shutdown(int s, int how)
{
    s_last_shut_down_socket = s;
    return 0;
}

int host_close(int s)
{
    s_closes++;
    s_last_closed_socket = s;
    return 0;
}
