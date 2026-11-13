// Host tests for the MySensors protocol (clu_proto/ms_proto.c) and controller
// (clu_mysensors.c, included: line assembly, handlers and Lua bindings are static) against a fake
// UART, NVS, clock and FreeRTOS queues. The Lua bindings run in a real Lua VM.
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// clu_mysensors.c reads the wall clock through time(): faked (the include guard keeps <time.h>
// from being parsed again with the macro in place)
static time_t s_time;
static time_t fake_time(time_t *t)
{
    if (t) {
        *t = s_time;
    }
    return s_time;
}
#define time(t) fake_time(t)

#include "../../components/clu_mysensors/clu_mysensors.c"

#undef time
#include "fake_nvs.h"
#include "lualib.h"
#include "tfbus_frame.h"
#include "tfbus_uart.h"
#include "ms_rs485.h"
#define MS_I_DISCOVER_REQUEST_FOR_TEST 20

// The firmware embeds mysensors.lua via EMBED_TXTFILES (NUL-terminated); test_mysensors.lua tests it.
__asm__(".pushsection .rodata\n"
        ".global _binary_mysensors_lua_start\n_binary_mysensors_lua_start:\n.asciz \"return 1\"\n"
        ".global _binary_mysensors_lua_end\n_binary_mysensors_lua_end:\n"
        ".popsection\n");

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- FreeRTOS
struct host_queue {
    size_t item, cap, head, n;
    uint8_t *buf;
};
static int s_queue_fail;
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item)
{
    if (s_queue_fail) {
        return NULL;
    }
    QueueHandle_t q = calloc(1, sizeof *q);
    q->item = item;
    q->cap = len;
    q->buf = calloc(len, item);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    if (q->n == q->cap) {
        return pdFALSE;
    }
    memcpy(q->buf + ((q->head + q->n) % q->cap) * q->item, item, q->item);
    q->n++;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    if (!q->n) {
        return pdFALSE;
    }
    memcpy(item, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->cap;
    q->n--;
    return pdTRUE;
}
static int s_task_fail, s_tasks;
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *h)
{
    s_tasks++;
    return s_task_fail ? pdFALSE : pdTRUE;
}

// ---------------------------------------------------------------- clock
static int64_t s_now_us;
int64_t esp_timer_get_time(void) { return s_now_us; }

// vTaskDelay (clu_mysensors_refresh waiting for the node check): time goes on, the UART task runs,
// and the nodes in s_answer answer the request sent to them (first frame of a presentation).
static void feed(const char *text);
static bool s_answer[256];
static bool s_uart_stuck; // the UART task does not run
void vTaskDelay(TickType_t t)
{
    s_now_us += (int64_t)t * 1000;
    if (s_uart_stuck) {
        return;
    }
    tick(s_now_us / 1000);
    if (s_probe_id && s_answer[s_probe_id] && s_now_us / 1000 > s_probe_sent) {
        char l[32];
        snprintf(l, sizeof l, "%d;255;3;0;15;0100\n", s_probe_id);
        feed(l);
    }
}

// ---------------------------------------------------------------- clu_lua
static const luaL_Reg *s_ext;
static char s_ext_name[16];
void clu_lua_register_ext(const luaL_Reg *fns, const char *src, size_t len, const char *name)
{
    s_ext = fns;
    snprintf(s_ext_name, sizeof s_ext_name, "%s", name);
}

// ---------------------------------------------------------------- tfbus_uart (bus endpoint)
static tfbus_uart_local_fn_t s_endpoint;
static int s_endpoints;
bool tfbus_uart_add_local(tfbus_uart_local_fn_t fn)
{
    s_endpoint = fn;
    s_endpoints++;
    return true;
}

// ---------------------------------------------------------------- fake UART
static struct {
    int installed, rxbuf, baud, tx, rx, rts, mode, waits;
    esp_err_t install_rc, pin_rc;
    int short_write;
} s_u;
static char s_out[4096]; // everything written (text)
static uint8_t s_bin[4096]; // everything written (binary: RS485 frames)
static size_t s_bin_len;

esp_err_t uart_driver_install(uart_port_t n, int rx, int tx, int qsize, void *q, int flags)
{
    s_u.installed++;
    s_u.rxbuf = rx;
    return s_u.install_rc;
}
esp_err_t uart_param_config(uart_port_t n, const uart_config_t *c)
{
    s_u.baud = c->baud_rate;
    return ESP_OK;
}
esp_err_t uart_set_pin(uart_port_t n, int tx, int rx, int rts, int cts)
{
    s_u.tx = tx;
    s_u.rx = rx;
    s_u.rts = rts;
    return s_u.pin_rc;
}
esp_err_t uart_set_mode(uart_port_t n, uart_mode_t mode)
{
    s_u.mode = mode;
    return ESP_OK;
}
int uart_write_bytes(uart_port_t n, const void *src, size_t len)
{
    if (s_u.short_write) {
        return (int)len - 1;
    }
    strncat(s_out, src, len);
    if (s_bin_len + len <= sizeof s_bin) {
        memcpy(s_bin + s_bin_len, src, len);
        s_bin_len += len;
    }
    return (int)len;
}
esp_err_t uart_wait_tx_done(uart_port_t n, TickType_t ticks)
{
    s_u.waits++;
    return ESP_OK;
}
// Feeds ms_task() one chunk, then ends the test's run of the task loop.
static const uint8_t *s_feed;
static size_t s_feed_len;
static jmp_buf s_task_exit;
int uart_read_bytes(uart_port_t n, void *buf, uint32_t len, TickType_t ticks)
{
    if (!s_feed) {
        longjmp(s_task_exit, 1);
    }
    memcpy(buf, s_feed, s_feed_len);
    s_feed = NULL;
    return (int)s_feed_len;
}

// ---------------------------------------------------------------- helpers
/** @brief Frame of a node's message (text line, node = sender) to the gateway. */
static size_t node_line_frame(const char *line, uint8_t *f, size_t cap)
{
    ms_msg_t m;
    if (!ms_parse(line, &m)) {
        printf("  test bug: bad line %s\n", line);
        return 0;
    }
    uint8_t sender = m.node, bin[MS_RS485_MAX_DATA];
    m.node = MS_GATEWAY;
    size_t n = ms_bin_encode(&m, sender, bin, sizeof bin);
    return ms_rs485_encode(MS_GATEWAY, sender, bin, n, f, cap);
}

/** @brief Nodes' messages ("node;child;cmd;ack;type;payload" lines, node = sender) through the bus receiver. */
static void feed(const char *text)
{
    char buf[2048], *save = NULL;
    snprintf(buf, sizeof buf, "%s", text);
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        uint8_t f[MS_RS485_MAX_FRAME];
        size_t n = node_line_frame(line, f, sizeof f);
        gw_bytes(f, n, s_now_us / 1000);
    }
}

/** @brief What the CLU put on the bus (send queue flushed), as text lines with node = destination. */
static int sent(const char *want)
{
    gw_flush(s_last_byte_ms + GW_IDLE_MS);
    char got[4096] = "";
    for (size_t i = 0; i + 9 <= s_bin_len;) {
        uint8_t len = s_bin[i + 4];
        ms_bin_t b;
        if (ms_bin_decode(s_bin + i + 6, len, &b)) {
            char line[MS_LINE_CAP];
            b.m.node = b.destination;
            b.m.ack = b.req_echo;
            if (ms_format(&b.m, line, sizeof line) > 0) {
                strncat(got, line, sizeof got - strlen(got) - 1);
            }
        }
        i += 9u + len;
    }
    s_bin_len = 0;
    s_out[0] = 0;
    int ok = !strcmp(got, want);
    if (!ok) {
        printf("  got  [%s]\n  want [%s]\n", got, want);
    }
    return ok;
}

/** @brief Flush the send queue and forget what went out. */
static void drain(void)
{
    gw_flush(s_last_byte_ms + GW_IDLE_MS);
    s_bin_len = 0;
    s_out[0] = 0;
}

static int lua_queued(void)
{
    return (int)s_luaq->n;
}

static void lua_flush(void)
{
    ms_msg_t m;
    while (xQueueReceive(s_luaq, &m, 0)) {
    }
}

static clu_mysensors_cfg_t s_base = {
    .uart_num = 1, .tx_pin = 17, .rx_pin = 18, .de_pin = -1, .baud = 57600, .metric = true,
};

static void reset_state(void)
{
    memset(s_nodes, 0, sizeof s_nodes);
    memset(s_children, 0, sizeof s_children);
    memset(s_values, 0, sizeof s_values);
    s_gw_version[0] = 0;
    s_rx = s_tx = s_dropped = s_bad = 0;
    s_out[0] = 0;
    memset(&s_win, 0, sizeof s_win);
    s_link = LINK_UNKNOWN;
    s_check_ms = 0;
    s_rx_bytes = 0;
    s_trace = false;
    s_gwq_head = s_gwq_n = 0;
    s_last_byte_ms = -1000;
    ms_rs485_rx_init(&s_frx, MS_GATEWAY);
    s_bin_len = 0;
    s_probe_next = 256; // no node check running
}

// ---------------------------------------------------------------- protocol
static void test_proto(void)
{
    ms_msg_t m;
    check("parse: set message", ms_parse("12;6;1;0;0;36.5", &m) && m.node == 12 && m.child == 6 && m.cmd == 1 &&
                                    m.ack == 0 && m.type == 0 && !strcmp(m.payload, "36.5"));
    check("parse: CR stripped, empty payload", ms_parse("0;255;3;1;2;\r", &m) && m.ack == 1 && !m.payload[0]);
    check("parse: payload keeps ';' (V_POSITION)",
          ms_parse("3;1;1;0;49;55.72;13.01;18", &m) && !strcmp(m.payload, "55.72;13.01;18"));
    char longl[200];
    snprintf(longl, sizeof longl, "0;255;3;0;9;%0100d", 7);
    check("parse: long payload cut to the buffer", ms_parse(longl, &m) && strlen(m.payload) == MS_PAYLOAD_CAP - 1);
    check("parse: 255 ok", ms_parse("255;255;3;0;3;", &m) && m.node == 255);
    const char *bad[] = {"", "1;2;3;0", "1;2;1;0;2", "a;2;1;0;2;x", "256;1;1;0;2;1", "0001;1;1;0;2;1",
                         "1;;1;0;2;1", "1;2;5;0;2;1", "1;2;1;2;2;1", "1 ;2;1;0;2;1", "-1;2;1;0;2;1"};
    int rejected = 1;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        if (ms_parse(bad[i], &m)) {
            printf("  accepted: %s\n", bad[i]);
            rejected = 0;
        }
    }
    check("parse: bad field count / number / range, cmd > 4, ack > 1 rejected", rejected);

    char out[64];
    ms_msg_t f = {.node = 13, .child = 7, .cmd = 1, .ack = 0, .type = 2, .payload = "1"};
    check("format: line with newline", ms_format(&f, out, sizeof out) == 13 && !strcmp(out, "13;7;1;0;2;1\n"));
    check("format: no room -> -1", ms_format(&f, out, 13) == -1);
    strcpy(f.payload, "a\nb");
    check("format: payload with a line break -> -1", ms_format(&f, out, sizeof out) == -1);
}

// ---------------------------------------------------------------- init
static void test_init(void)
{
    s_u.install_rc = ESP_FAIL;
    check("init: UART install failure reported", clu_mysensors_init(&s_base) == ESP_FAIL && !s_tasks);
    s_u.install_rc = ESP_OK;
    s_u.pin_rc = ESP_ERR_INVALID_ARG;
    check("init: pin failure reported", clu_mysensors_init(&s_base) == ESP_ERR_INVALID_ARG);
    s_u.pin_rc = ESP_OK;
    s_queue_fail = 1;
    check("init: no memory for the queues", clu_mysensors_init(&s_base) == ESP_ERR_NO_MEM);
    s_queue_fail = 0;
    s_task_fail = 1;
    check("init: task not created", clu_mysensors_init(&s_base) == ESP_ERR_NO_MEM);
    s_task_fail = 0;

    // ids from NVS (node 1 and 2 already handed out)
    nvs_handle_t h;
    nvs_open(NVS_NS, NVS_READWRITE, &h);
    uint8_t ids[32] = {0x06};
    nvs_set_blob(h, NVS_IDS, ids, sizeof ids);
    check("init: plain UART without a DE pin (auto-direction transceiver)",
          clu_mysensors_init(&s_base) == ESP_OK && s_u.mode == UART_MODE_UART && s_u.baud == 57600 && s_u.tx == 17 &&
              s_u.rx == 18 && s_u.rts == -1 && s_u.rxbuf == 1024 && s_tasks == 2);
    check("init: Lua extension registered", s_ext == s_fns && !strcmp(s_ext_name, "=mysensors"));
    check("init: node ids from NVS", id_taken(1) && id_taken(2) && !id_taken(3));
    check("init: the gateway runs (connected), version", clu_mysensors_connected() &&
                                                           !strcmp(s_gw_version, "CLU RS485 gateway"));

    clu_mysensors_cfg_t rs = s_base;
    rs.de_pin = 21;
    clu_mysensors_init(&rs);
    check("init: RS485 half duplex with a DE pin", s_u.mode == UART_MODE_RS485_HALF_DUPLEX && s_u.rts == 21);
    nvs_set_blob(h, NVS_IDS, ids, 5);
    ids_load();
    check("init: short NVS blob ignored", !id_taken(1));
    nvs_fail_open = 1;
    ids_load();
    check("init: NVS unavailable -> no ids", !id_taken(1));
    nvs_fail_open = 0;
    nvs_set_blob(h, NVS_IDS, ids, sizeof ids);
    clu_mysensors_init(&s_base);
}

// ---------------------------------------------------------------- controller
static void test_ids(void)
{
    reset_state();
    feed("255;255;3;0;3;\n");
    check("I_ID_REQUEST: lowest free id, broadcast", sent("255;255;3;0;4;3\n"));
    drain();
    feed("255;212;3;0;3;\n");
    check("I_ID_REQUEST: the node's token echoed", sent("255;212;3;0;4;4\n"));
    const nvs_entry_t *e = nvs_peek(NVS_NS, NVS_IDS);
    check("I_ID_REQUEST: id saved in NVS", e && e->len == 32 && (e->val[0] & 0x08));
    feed("5;255;3;0;11;Fixed\n");
    lua_flush();
    feed("255;255;3;0;3;\n255;255;3;0;3;\n");
    check("node with a fixed id: its id is taken too", sent("255;255;3;0;4;6\n255;255;3;0;4;7\n") && id_taken(5));
    ids_load();
    check("ids survive a reboot", id_taken(3) && id_taken(4) && id_taken(5) && id_taken(6) && id_taken(7) && !id_taken(8));
    memset(s_ids, 0xff, sizeof s_ids);
    feed("255;255;3;0;3;\n");
    check("all ids taken: no answer", sent(""));
    memset(s_ids, 0, sizeof s_ids);
    nvs_fail_open = 1;
    feed("255;255;3;0;3;\n");
    check("NVS unavailable: id still handed out", sent("255;255;3;0;4;1\n"));
    nvs_fail_open = 0;
    lua_flush();
}

static void test_internal(void)
{
    reset_state();
    feed("12;255;3;0;6;0\n");
    check("I_CONFIG: metric", sent("12;255;3;0;6;M\n"));
    s_cfg.metric = false;
    feed("12;255;3;0;6;0\n");
    check("I_CONFIG: imperial", sent("12;255;3;0;6;I\n"));
    s_cfg.metric = true;

    setenv("TZ", "CET-1", 1);
    tzset();
    s_time = 1700000000; // 2023-11-14 22:13:20 UTC
    feed("12;255;3;0;1;\n");
    check("I_TIME: local time (CET = UTC+1)", sent("12;255;3;0;1;1700003600\n"));
    setenv("TZ", "UTC0", 1);
    tzset();
    s_time = 951782400; // 2000-02-29: before 2021, clock not set
    feed("12;255;3;0;1;\n");
    check("I_TIME: clock not set -> no answer", sent(""));
    s_time = 1709164800; // 2024-02-29
    feed("12;255;3;0;1;\n");
    check("I_TIME: leap day", sent("12;255;3;0;1;1709164800\n"));

    lua_flush();
    feed("12;255;3;0;9;TSF:MSG:READ\n");
    check("I_LOG_MESSAGE: not passed to the scripts", lua_queued() == 0 && sent(""));

    feed("12;255;3;0;11;Weather\n12;255;3;0;12;1.2\n12;255;3;0;0;87\n13;255;3;0;0;150\n14;255;3;0;0;-4\n"
         "255;255;3;0;11;Nobody\n12;255;3;0;22;\n");
    const ms_node_t *n = node_find(12);
    check("sketch name / version / battery kept",
          n && !strcmp(n->sketch, "Weather") && !strcmp(n->version, "1.2") && n->battery == 87);
    check("battery clamped to 0..100", node_find(13)->battery == 100 && node_find(14)->battery == 0);
    check("nothing kept for the broadcast id", !node_find(255));
    lua_flush();
}

static void test_values(void)
{
    reset_state();
    feed("12;255;0;0;17;2.3.2\n12;1;0;0;6;Outside\n12;1;0;0;7;Updated\n255;255;0;0;17;x\n");
    const ms_node_t *n = node_find(12);
    check("node presentation: type and library", n && n->type == 17 && !strcmp(n->lib, "2.3.2"));
    check("child presentation kept / updated", s_children[0].used && s_children[0].child == 1 &&
                                                   s_children[0].type == 7 && !strcmp(s_children[0].desc, "Updated"));

    feed("12;1;1;0;0;21.5\n");
    feed("12;1;2;0;0;\n");
    check("C_REQ answered from the last value", sent("12;1;1;0;0;21.5\n"));
    feed("12;1;2;0;1;\n");
    check("C_REQ for an unknown value: no answer", sent(""));
    ms_msg_t m = {.node = 13, .child = 7, .cmd = MS_C_SET, .type = 2, .payload = "1"};
    check("send: queued", clu_mysensors_send(&m));
    feed("13;7;2;0;2;\n");
    check("C_REQ answered with the value a script set", sent("13;7;1;0;2;1\n"));
    tick(100000);
    check("tick: queued script messages go out", sent("13;7;1;0;2;1\n"));
    tick(100001);
    check("tick: nothing more", sent(""));

    feed("12;1;4;0;0;0100\n");
    check("C_STREAM: passed on, nothing answered", sent(""));
    lua_flush();

    ms_msg_t big = {.node = 1, .cmd = MS_C_SET};
    memset(big.payload, 'x', 26);
    check("send: payload over 25 bytes refused", !clu_mysensors_send(&big));
    strcpy(big.payload, "a\nb");
    check("send: line break refused", !clu_mysensors_send(&big));
    m.cmd = MS_C_REQ;
    int queued = 0;
    while (clu_mysensors_send(&m)) {
        queued++;
    }
    check("send: queue full -> false", queued == MS_TX_QUEUE);
    tick(600000);
    drain();
}

static void test_tables(void)
{
    reset_state();
    // nodes: least recently seen dropped
    for (int i = 1; i <= MS_MAX_NODES + 1; i++) {
        char l[48];
        s_now_us = (int64_t)i * 1000;
        snprintf(l, sizeof l, "%d;255;3;0;22;\n", i);
        feed(l);
    }
    check("node registry full: least recently seen dropped", !node_find(1) && node_find(2) && node_find(MS_MAX_NODES + 1));
    // values: oldest dropped
    for (int i = 0; i <= MS_MAX_VALUES; i++) {
        char l[48];
        s_now_us = (int64_t)(100 + i) * 1000;
        snprintf(l, sizeof l, "40;%d;1;0;0;%d\n", i, i);
        feed(l);
        lua_flush();
    }
    check("value cache full: oldest dropped", !value_find(40, 0, 0) && value_find(40, MS_MAX_VALUES, 0));
    // children: full table refuses
    for (int i = 0; i <= MS_MAX_CHILDREN; i++) {
        char l[48];
        snprintf(l, sizeof l, "41;%d;0;0;6;t\n", i);
        feed(l);
        lua_flush();
    }
    int n = 0;
    for (int i = 0; i < MS_MAX_CHILDREN; i++) {
        n += s_children[i].node == 41;
    }
    check("child table full: new children not kept", n == MS_MAX_CHILDREN);
    s_out[0] = 0;
    s_now_us = 0;
}

static void test_flow(void)
{
    reset_state();
    feed("12;1;1;0;0;20.5\n");
    check("message passed to the scripts", s_rx == 1 && lua_queued() == 1 && !strcmp(value_find(12, 1, 0)->payload, "20.5"));
    for (int i = 0; i < MS_LUA_QUEUE + 2; i++) {
        feed("12;1;1;0;0;1\n");
    }
    check("scripts not keeping up: messages dropped, counted", s_dropped == 3);
    lua_flush();

    // messages the gateway cannot send
    unsigned tx = s_tx;
    ms_msg_t big = {.node = 1, .cmd = MS_C_SET};
    memset(big.payload, 'x', 30);
    wire(&big);
    ms_msg_t self = {.node = MS_GATEWAY, .cmd = MS_C_SET, .payload = "1"};
    wire(&self);
    check("too long / to the gateway itself: not sent", s_gwq_n == 0 && s_tx == tx && sent(""));
}

static void test_task(void)
{
    reset_state();
    uint8_t f[MS_RS485_MAX_FRAME];
    s_feed_len = node_line_frame("12;255;3;0;24;1", f, sizeof f); // I_PING
    s_feed = f;
    if (!setjmp(s_task_exit)) {
        ms_task(NULL);
    }
    check("task: reads the UART, handles frames, ticks (pong sent once the bus is quiet)",
          s_rx == 0 && s_gwq_n == 1 && sent("12;255;3;0;25;1\n"));
    lua_flush();
}

// ---------------------------------------------------------------- Lua bindings
static lua_State *L;

static int run(const char *chunk)
{
    if (luaL_dostring(L, chunk) != LUA_OK) {
        printf("  lua: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    int ok = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return ok;
}

static void test_lua(void)
{
    reset_state();
    L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    luaL_setfuncs(L, s_fns, 0);
    lua_setglobal(L, "__clu");

    check("lua: ms_recv empty -> nil", run("return __clu.ms_recv() == nil"));
    feed("12;1;1;0;0;21.5\n");
    check("lua: ms_recv returns the message",
          run("local n,c,cmd,a,t,p = __clu.ms_recv(); return n==12 and c==1 and cmd==1 and a==0 and t==0 and p=='21.5'"));
    s_now_us = 2500 * 1000;
    check("lua: ms_get -> payload, age", run("local p, age = __clu.ms_get(12, 1, 0); return p == '21.5' and age == 2500"));
    check("lua: ms_get unknown -> nil", run("return __clu.ms_get(12, 1, 9) == nil"));
    check("lua: ms_send queued, value cached",
          run("return __clu.ms_send(13, 7, 1, 2, '1', true) and select(1, __clu.ms_get(13, 7, 2)) == '1'"));
    tick(10000);
    check("lua: ms_send with echo request goes out", sent("13;7;1;1;2;1\n"));
    check("lua: ms_send payload optional", run("return __clu.ms_send(13, 255, 3, 2)"));
    tick(10001);
    check("lua: no payload -> P_BYTE 0", sent("13;255;3;0;2;0\n"));
    check("lua: node out of range", run("return not pcall(__clu.ms_send, 256, 1, 1, 2, '1')"));
    check("lua: command out of range", run("return not pcall(__clu.ms_send, 1, 1, 5, 2, '1')"));
    check("lua: payload too long", run("return not pcall(__clu.ms_send, 1, 1, 1, 2, ('x'):rep(26))"));
    check("lua: payload with NUL / newline", run("return not pcall(__clu.ms_send, 1, 1, 1, 2, 'a\\0b') and "
                                                 "not pcall(__clu.ms_send, 1, 1, 1, 2, 'a\\nb')"));

    feed("12;255;0;0;17;2.3.2\n12;255;3;0;11;Weather\n12;255;3;0;0;87\n12;1;0;0;6;Outside\n14;255;3;0;22;\n");
    memset(&s_nodes[5], 0, sizeof s_nodes[5]); // child of a node no longer in the registry
    s_children[3] = (ms_child_t){.used = true, .node = 99, .child = 1, .type = 6};
    check("lua: ms_nodes",
          run("local t = __clu.ms_nodes(); local n = t[12]; "
              "return n.sketch == 'Weather' and n.lib == '2.3.2' and n.type == 17 and n.battery == 87 and "
              "n.version == '' and n.age_ms == 0 and n.children[1].type == 6 and n.children[1].desc == 'Outside' and "
              "t[14].battery == nil and next(t[14].children) == nil and t[99] == nil"));
    check("lua: ms_status",
          run("local s = __clu.ms_status(); return s.connected and s.version == '' and s.rx == 6 and s.tx == 2 and "
              "s.dropped == 0 and s.bad == 0 and s.rx_bytes > 0 and s.link == 'ok' and s.trace == false"));
    check("lua: ms_trace switches tracing", run("__clu.ms_trace(true); return __clu.ms_status().trace") && s_trace);
    s_trace = false;
    lua_close(L);
    lua_flush();
}

// ---------------------------------------------------------------- RS485 network (MY_RS485)
static void test_rs485_proto(void)
{
    ms_bin_t b0;
    // frame: SOH to from 'X' len STX data ETX cs EOT
    uint8_t f[MS_RS485_MAX_FRAME];
    const uint8_t d[3] = {0x10, 0x20, 0xf0};
    size_t n = ms_rs485_encode(12, 0, d, 3, f, sizeof f);
    const uint8_t want[] = {1, 12, 0, 0x58, 3, 2, 0x10, 0x20, 0xf0, 3, (uint8_t)(12 + 0x58 + 3 + 0x10 + 0x20 + 0xf0), 4};
    check("rs485: frame layout and checksum", n == sizeof want && !memcmp(f, want, n));
    check("rs485: too long / no room -> 0", ms_rs485_encode(1, 0, d, 41, f, sizeof f) == 0 &&
                                               ms_rs485_encode(1, 0, d, 3, f, 11) == 0);

    ms_rs485_rx_t rx;
    ms_rs485_frame_t got;
    int r = 0;
    ms_rs485_rx_init(&rx, 12);
    const uint8_t noise[] = {0xff, 1, 7, 0x00, 2};
    for (size_t i = 0; i < sizeof noise; i++) {
        r |= ms_rs485_rx_push(&rx, noise[i], &got);
    }
    for (size_t i = 0; i < n; i++) {
        r = ms_rs485_rx_push(&rx, f[i], &got);
    }
    check("rs485: frame found after noise", r == 1 && got.to == 12 && got.from == 0 && got.len == 3 && got.data[2] == 0xf0);

    // as the gateway (0): frames for other nodes, from ourselves, to == from, too long: skipped
    ms_rs485_rx_init(&rx, 0);
    int seen = 0;
    uint8_t g[MS_RS485_MAX_FRAME];
    size_t gl;
    const struct { uint8_t to, from; } skip[] = {{12, 3}, {255, 0}};
    for (size_t k = 0; k < 2; k++) {
        gl = ms_rs485_encode(skip[k].to, skip[k].from, d, 3, g, sizeof g);
        for (size_t i = 0; i < gl; i++) {
            seen += ms_rs485_rx_push(&rx, g[i], &got) != 0;
        }
    }
    const uint8_t same[] = {1, 5, 5, 0x58, 0, 2, 3, 0xba, 4};
    const uint8_t longf[] = {1, 0, 5, 0x58, 40, 2};
    for (size_t i = 0; i < sizeof same; i++) {
        seen += ms_rs485_rx_push(&rx, same[i], &got) != 0;
    }
    for (size_t i = 0; i < sizeof longf; i++) {
        seen += ms_rs485_rx_push(&rx, longf[i], &got) != 0;
    }
    check("rs485: other node / own / to == from / too long: skipped", seen == 0);
    gl = ms_rs485_encode(255, 7, NULL, 0, g, sizeof g);
    for (size_t i = 0; i < gl; i++) {
        r = ms_rs485_rx_push(&rx, g[i], &got);
    }
    check("rs485: empty broadcast accepted", r == 1 && got.to == 255 && got.len == 0);
    // captured from a node without an id (SOH x3, to = from = 255): its I_FIND_PARENT_REQUEST
    const uint8_t fpar[] = {1, 1, 1, 0xff, 0xff, 0x58, 7, 2, 0xff, 0xff, 0xff, 2, 3, 7, 0xff, 3, 0x65, 4};
    for (size_t i = 0; i < sizeof fpar; i++) {
        r = ms_rs485_rx_push(&rx, fpar[i], &got);
    }
    check("rs485: node without an id (to = from = 255, 3x SOH) accepted",
          r == 1 && got.to == 255 && got.from == 255 && got.len == 7 && ms_bin_decode(got.data, got.len, &b0) &&
              b0.m.cmd == MS_C_INTERNAL && b0.m.type == MS_I_FIND_PARENT_REQUEST && b0.sender == 255);
    gl = ms_rs485_encode(0, 7, d, 3, g, sizeof g);
    g[gl - 2] ^= 1;
    for (size_t i = 0; i < gl; i++) {
        r = ms_rs485_rx_push(&rx, g[i], &got);
    }
    check("rs485: bad checksum -> -1", r == -1);
    gl = ms_rs485_encode(0, 7, d, 3, g, sizeof g);
    g[gl - 3] = 0x55;
    for (size_t i = 0; i < gl - 2; i++) {
        r = ms_rs485_rx_push(&rx, g[i], &got);
    }
    check("rs485: no ETX -> -1", r == -1);
    gl = ms_rs485_encode(0, 7, d, 3, g, sizeof g);
    g[gl - 1] = 0x55;
    for (size_t i = 0; i < gl; i++) {
        r = ms_rs485_rx_push(&rx, g[i], &got);
    }
    check("rs485: no EOT -> -1", r == -1);
    const uint8_t other_cmd[] = {1, 0, 7, 0x41, 0, 2, 3, (uint8_t)(7 + 0x41), 4};
    for (size_t i = 0; i < sizeof other_cmd; i++) {
        r = ms_rs485_rx_push(&rx, other_cmd[i], &got);
    }
    check("rs485: valid frame of another command: ignored", r == 0);

    // MyMessage payloads -> text, as the serial gateway prints them
    ms_bin_t b;
    struct {
        uint8_t ptype;
        uint8_t data[6];
        uint8_t len;
        const char *text;
    } pt[] = {
        {MS_P_STRING, "hi\n", 3, "hi "}, {MS_P_BYTE, {200}, 1, "200"}, {MS_P_INT16, {0xfe, 0xff}, 2, "-2"},
        {MS_P_UINT16, {0xfe, 0xff}, 2, "65534"}, {MS_P_LONG32, {0xff, 0xff, 0xff, 0xff}, 4, "-1"},
        {MS_P_ULONG32, {0x40, 0xe2, 0x01, 0}, 4, "123456"}, {MS_P_FLOAT32, {0, 0, 0xac, 0x41, 1}, 5, "21.5"},
        {MS_P_FLOAT32, {0, 0, 0xa0, 0x40, 0}, 5, " 5"}, {MS_P_FLOAT32, {0, 0, 0x80, 0x3f, 20}, 5, "1.00000000"},
        {MS_P_CUSTOM, {0xab, 0x01}, 2, "AB01"},
    };
    int ok = 1;
    for (size_t k = 0; k < sizeof pt / sizeof *pt; k++) {
        uint8_t m[MS_BIN_HEADER + 6] = {12, 12, 0, (uint8_t)(2 | pt[k].len << 3), (uint8_t)(1 | pt[k].ptype << 5), 0, 1};
        memcpy(m + MS_BIN_HEADER, pt[k].data, pt[k].len);
        if (!ms_bin_decode(m, MS_BIN_HEADER + pt[k].len, &b) || strcmp(b.m.payload, pt[k].text)) {
            printf("  ptype %u: [%s] want [%s]\n", pt[k].ptype, b.m.payload, pt[k].text);
            ok = 0;
        }
    }
    check("bin: every payload type rendered like getString()", ok);
    const uint8_t hdr[] = {3, 12, 0, (uint8_t)(2 | 3 << 3), (uint8_t)(3 | 1 << 3 | 1 << 4), 24, 7};
    check("bin: header fields", ms_bin_decode(hdr, sizeof hdr, &b) && b.last == 3 && b.sender == 12 && b.destination == 0 &&
                                    b.req_echo && b.is_echo && b.m.cmd == 3 && b.m.type == 24 && b.m.child == 7 &&
                                    b.m.node == 12 && b.m.ack == 1 && !b.m.payload[0]);
    const uint8_t bad_ver[] = {0, 1, 0, 1, 1, 0, 0}, bad_cmd[] = {0, 1, 0, 2, 7, 0, 0};
    const uint8_t long_len[] = {0, 1, 0, (uint8_t)(2 | 31 << 3), 1, 0, 0, 'a', 'b'};
    check("bin: too short / version != 2 / command > 4 rejected", !ms_bin_decode(hdr, 6, &b) && !ms_bin_decode(bad_ver, 7, &b) &&
                                                                     !ms_bin_decode(bad_cmd, 7, &b));
    check("bin: length beyond the frame cut to the frame", ms_bin_decode(long_len, sizeof long_len, &b) && !strcmp(b.m.payload, "ab"));

    // controller -> MyMessage
    uint8_t o[64];
    ms_msg_t m = {.node = 13, .child = 7, .cmd = MS_C_SET, .ack = 1, .type = 2, .payload = "1"};
    n = ms_bin_encode(&m, 0, o, sizeof o);
    check("bin encode: text payload as P_STRING, echo request", n == 8 && o[1] == 0 && o[2] == 13 && o[3] == (2 | 1 << 3) &&
                                                                  o[4] == (1 | 1 << 3) && o[5] == 2 && o[6] == 7 && o[7] == '1');
    m.payload[0] = 0;
    m.ack = 0;
    n = ms_bin_encode(&m, 0, o, sizeof o);
    check("bin encode: no payload -> P_BYTE 0", n == 8 && o[4] == (1 | MS_P_BYTE << 5) && o[7] == 0);
    m.cmd = MS_C_STREAM;
    strcpy(m.payload, "aB0f");
    n = ms_bin_encode(&m, 0, o, sizeof o);
    check("bin encode: C_STREAM hex -> P_CUSTOM bytes", n == 9 && o[4] == (4 | MS_P_CUSTOM << 5) && o[7] == 0xab && o[8] == 0x0f);
    memset(m.payload, 'a', 60);
    m.payload[60] = 0;
    check("bin encode: stream too long -> 0", ms_bin_encode(&m, 0, o, sizeof o) == 0);
    m.cmd = MS_C_SET;
    m.payload[26] = 0;
    check("bin encode: text too long / no room -> 0", ms_bin_encode(&m, 0, o, sizeof o) == 0 &&
                                                         (strcpy(m.payload, "12"), ms_bin_encode(&m, 0, o, 8) == 0));
    check("bin encode: one byte answer; no room -> 0",
          ms_bin_encode_byte(0, 5, 255, 3, MS_I_PONG, 1, o, sizeof o) == 8 && o[2] == 5 && o[5] == MS_I_PONG && o[7] == 1 &&
              ms_bin_encode_byte(0, 5, 255, 3, MS_I_PONG, 1, o, 7) == 0);
}

/** @brief A node's frame to the gateway (dest 0) or everyone (255), fed to the CLU's receiver. */
static void node_frame(uint8_t to, uint8_t from, uint8_t child, uint8_t cmd, uint8_t type, uint8_t ptype,
                       const void *payload, uint8_t plen, bool req_echo)
{
    uint8_t m[MS_RS485_MAX_DATA] = {from, from, to, (uint8_t)(2 | plen << 3),
                                    (uint8_t)(cmd | (req_echo ? 8 : 0) | ptype << 5), type, child};
    memcpy(m + MS_BIN_HEADER, payload, plen);
    uint8_t f[MS_RS485_MAX_FRAME];
    size_t n = ms_rs485_encode(to, from, m, MS_BIN_HEADER + plen, f, sizeof f);
    gw_bytes(f, n, s_now_us / 1000);
}

/** @brief Frames the CLU wrote, decoded as node @p as would: count, last one in *last. */
static int sent_frames(uint8_t as, ms_bin_t *last, uint8_t *to)
{
    ms_rs485_rx_t rx;
    ms_rs485_rx_init(&rx, as);
    ms_rs485_frame_t f;
    int n = 0;
    for (size_t i = 0; i < s_bin_len; i++) {
        if (ms_rs485_rx_push(&rx, s_bin[i], &f) == 1 && ms_bin_decode(f.data, f.len, last)) {
            *to = f.to;
            n++;
        }
    }
    s_bin_len = 0;
    s_out[0] = 0;
    return n;
}

static void test_gateway(void)
{
    reset_state();
    clu_mysensors_init(&s_base);
    ms_bin_t b;
    uint8_t to = 0;
    s_now_us = 1000 * 1000;
    check("gateway: connected (the CLU is the gateway), version", clu_mysensors_connected() &&
                                                                    !strcmp(s_gw_version, "CLU RS485 gateway"));

    // node boot: find parent (broadcast) -> distance 0, answered once the bus is quiet
    uint8_t zero = 0;
    node_frame(255, 12, 255, MS_C_INTERNAL, MS_I_FIND_PARENT_REQUEST, MS_P_BYTE, &zero, 1, false);
    tick(1002);
    check("gateway: nothing sent while the bus is busy", s_bin_len == 0 && s_gwq_n == 1);
    tick(1010);
    check("gateway: find parent -> I_FIND_PARENT_RESPONSE 0 to the node",
          sent_frames(12, &b, &to) == 1 && to == 12 && b.sender == 0 && b.m.cmd == MS_C_INTERNAL &&
              b.m.type == MS_I_FIND_PARENT_RESPONSE && !strcmp(b.m.payload, "0") && lua_queued() == 0);
    const uint8_t fpar[] = {1, 1, 1, 0xff, 0xff, 0x58, 7, 2, 0xff, 0xff, 0xff, 2, 3, 7, 0xff, 3, 0x65, 4};
    gw_bytes(fpar, sizeof fpar, 1500);
    tick(1600);
    check("gateway: captured find parent of a node without an id -> answer broadcast to 255",
          sent_frames(200, &b, &to) == 1 && to == 255 && b.destination == 255 && b.m.type == MS_I_FIND_PARENT_RESPONSE);
    node_frame(255, 12, 255, MS_C_INTERNAL, MS_I_DISCOVER_REQUEST_FOR_TEST, MS_P_BYTE, &zero, 1, false);
    tick(2000);
    check("gateway: other internal broadcasts are for the nodes", sent_frames(12, &b, &to) == 0 && lua_queued() == 0);

    // id request from an unassigned node (255) -> controller -> broadcast I_ID_RESPONSE
    node_frame(0, 255, 7, MS_C_INTERNAL, MS_I_ID_REQUEST, MS_P_STRING, "", 0, false);
    tick(3000);
    check("gateway: I_ID_REQUEST answered by the controller, broadcast with the new id and the token",
          sent_frames(200, &b, &to) == 1 && to == 255 && b.m.type == MS_I_ID_RESPONSE && b.m.child == 7 &&
              atoi(b.m.payload) >= 1);
    lua_flush();

    // ping, registration: the gateway's own answers
    uint8_t one = 1;
    node_frame(0, 12, 255, MS_C_INTERNAL, MS_I_PING, MS_P_BYTE, &one, 1, false);
    tick(4000);
    check("gateway: I_PING -> I_PONG 1", sent_frames(12, &b, &to) == 1 && b.m.type == MS_I_PONG && !strcmp(b.m.payload, "1"));
    uint8_t ver = 2;
    node_frame(0, 12, 255, MS_C_INTERNAL, MS_I_REGISTRATION_REQUEST, MS_P_BYTE, &ver, 1, false);
    tick(5000);
    check("gateway: registration approved, not passed on",
          sent_frames(12, &b, &to) == 1 && b.m.type == MS_I_REGISTRATION_RESPONSE && !strcmp(b.m.payload, "1") &&
              lua_queued() == 0);
    node_frame(0, 12, 255, MS_C_INTERNAL, MS_I_PONG, MS_P_BYTE, &one, 1, false);
    node_frame(0, 12, 255, MS_C_INTERNAL, MS_I_SIGNAL_REPORT_REVERSE, MS_P_BYTE, &one, 1, false);
    tick(6000);
    check("gateway: pong / signal report reverse: dropped", sent_frames(12, &b, &to) == 0 && lua_queued() == 0);

    // presentation, values, config: through the controller
    node_frame(0, 12, 1, MS_C_PRESENTATION, 3, MS_P_STRING, "Relay", 5, false);
    uint8_t on = 1;
    node_frame(0, 12, 1, MS_C_SET, 2, MS_P_BYTE, &on, 1, true);
    tick(7000);
    check("gateway: C_SET with echo request: echo back to the node, value kept, scripts see it",
          sent_frames(12, &b, &to) == 1 && b.is_echo && !b.req_echo && b.sender == 0 && b.destination == 12 &&
              b.m.cmd == MS_C_SET && value_find(12, 1, 2) && !strcmp(value_find(12, 1, 2)->payload, "1") && lua_queued() == 2);
    lua_flush();
    node_frame(0, 12, 255, MS_C_INTERNAL, MS_I_CONFIG, MS_P_STRING, "", 0, false);
    tick(8000);
    check("gateway: I_CONFIG -> M", sent_frames(12, &b, &to) == 1 && b.m.type == MS_I_CONFIG && !strcmp(b.m.payload, "M"));
    node_frame(0, 12, 1, MS_C_SET, 2, MS_P_BYTE, &on, 1, false);
    s_bin_len = 0;
    uint8_t echo_flags = (uint8_t)(1 | 1 << 4 | MS_P_BYTE << 5); // an echo from the node: no echo back
    {
        uint8_t m[] = {12, 12, 0, (uint8_t)(2 | 1 << 3), echo_flags, 2, 1, 0};
        uint8_t f[MS_RS485_MAX_FRAME];
        size_t n = ms_rs485_encode(0, 12, m, sizeof m, f, sizeof f);
        gw_bytes(f, n, 8500);
    }
    tick(9000);
    check("gateway: echoes from nodes go to the controller as ack (I_CONFIG, C_SET, echo)",
          sent_frames(12, &b, &to) == 0 && lua_queued() == 3);
    lua_flush();
    node_frame(255, 12, 1, MS_C_SET, 16, MS_P_BYTE, &on, 1, false);
    check("gateway: non-internal broadcast handed over", lua_queued() == 1 && value_find(12, 1, 16));
    lua_flush();

    // script / OM output -> frame to the node
    ms_msg_t m = {.node = 12, .child = 1, .cmd = MS_C_SET, .type = 2, .payload = "0"};
    clu_mysensors_send(&m);
    m.node = 0;
    clu_mysensors_send(&m);
    s_trace = true;
    tick(10000);
    s_trace = false;
    check("gateway: C_SET to the node as P_STRING; to the gateway itself: not sent",
          sent_frames(12, &b, &to) == 1 && to == 12 && b.m.cmd == MS_C_SET && b.ptype == MS_P_STRING && !strcmp(b.m.payload, "0"));

    // broken frames, garbage, link check
    unsigned bad = s_bad;
    uint8_t f[MS_RS485_MAX_FRAME];
    uint8_t junk[] = {12, 12, 0, 1, 1, 0, 0}; // version 1
    size_t n = ms_rs485_encode(0, 12, junk, sizeof junk, f, sizeof f);
    gw_bytes(f, n, 11000);
    f[n - 2] ^= 0xff;
    gw_bytes(f, n, 11000);
    check("gateway: undecodable message / bad checksum counted", s_bad == bad + 2);
    s_link = LINK_UNKNOWN;
    memset(&s_win, 0, sizeof s_win);
    s_check_ms = 0;
    gw_bytes((const uint8_t *)"\x93\xf1\x00", 3, 12000);
    tick(12000);
    check("gateway: bytes without a frame -> garbage", s_link == LINK_GARBAGE && s_check_ms == 12000 + GW_CHECK_MS);
    node_frame(0, 12, 1, MS_C_SET, 2, MS_P_BYTE, &on, 1, false);
    tick(12000 + GW_CHECK_MS);
    check("gateway: frames -> ok", s_link == LINK_OK);
    tick(12000 + 2 * GW_CHECK_MS);
    check("gateway: quiet bus: not judged", s_link == LINK_OK);
    lua_flush();

    // send queue full; UART failure
    s_last_byte_ms = 50000;
    for (int i = 0; i < GW_QUEUE + 1; i++) {
        gw_answer(12, MS_I_PONG, 1);
    }
    unsigned dropped = s_dropped;
    check("gateway: queue full -> dropped", s_gwq_n == GW_QUEUE && dropped >= 1);
    s_u.short_write = 1;
    unsigned tx = s_tx;
    gw_flush(60000);
    s_u.short_write = 0;
    check("gateway: UART write failure: not counted", s_tx == tx && s_gwq_n == 0);
    s_cfg.de_pin = 21;
    gw_answer(12, MS_I_PONG, 1);
    gw_flush(70000);
    s_cfg.de_pin = -1;
    check("gateway: RS485: waits for the frame to leave", s_tx == tx + 1);

    s_bin_len = 0;
    s_out[0] = 0;
    s_now_us = 0;
    lua_flush();
}

// ---------------------------------------------------------------- emulated modules
static uint8_t f[TFBUS_MAX_FRAME], frx[TFBUS_MAX_FRAME];

static int bus(size_t fl)
{
    memset(frx, 0, sizeof frx);
    return s_endpoint(f, fl, frx, sizeof frx);
}

static bool is_ack(int n)
{
    return n == 4 && frx[0] == 0xaa && frx[2] == 0x01;
}

/** @brief DISCOVER: serial of the module that answered, 0 if none. */
static uint32_t discover(uint8_t *hw)
{
    int n = bus(tfbus_discover(f, sizeof f));
    if (n != 11) {
        return 0;
    }
    *hw = frx[7];
    return frx[3] | frx[4] << 8 | frx[5] << 16 | (uint32_t)frx[6] << 24;
}

/** @brief STATUS_POLL: inputs / outputs bytes; false if no reply. */
static bool poll(uint8_t addr, uint8_t *in, uint8_t *out)
{
    int n = bus(tfbus_status_poll(f, sizeof f, addr));
    if (n != 13) {
        return false;
    }
    *in = frx[7];
    *out = frx[11];
    return true;
}

static int module_cmd(uint8_t addr, uint8_t type, uint8_t mask)
{
    const tfbus_tlv_t rec = {.type = type, .meta = 0x01, .len = 1, .data = &mask};
    return bus(tfbus_module_cmd(f, sizeof f, addr, &rec, 1));
}

static int nslots(void)
{
    ms_vmod_slot_t v[64];
    return ms_vmod_list(v, 64);
}

/** @brief Binding of node/child (kind), -1 module if none. */
static int slot_of(uint8_t node, uint8_t child, int *module, int *channel, ms_slot_kind_t *kind)
{
    ms_vmod_slot_t v[MS_VMOD_MAX * MS_VMOD_SLOTS];
    int n = ms_vmod_list(v, MS_VMOD_MAX * MS_VMOD_SLOTS);
    for (int i = 0; i < n; i++) {
        if (v[i].node == node && v[i].child == child) {
            *module = v[i].module;
            *channel = v[i].channel;
            *kind = v[i].kind;
            return 1;
        }
    }
    *module = *channel = -1;
    return 0;
}

static bool at(uint8_t node, uint8_t child, int module, ms_slot_kind_t kind, int channel)
{
    int m, c;
    ms_slot_kind_t k;
    return slot_of(node, child, &m, &c, &k) && m == module && k == kind && c == channel;
}

static void test_vmod(void)
{
    reset_state();
    lua_flush();
    check("no modules configured: nothing bound / listed", nslots() == 0 && ms_vmod_unmap(12, 255) == 0 &&
                                                              ms_vmod_frame(f, tfbus_discover(f, sizeof f), frx, sizeof frx) == 0);
    feed("12;1;0;0;0;Door\n");
    check("no modules configured: presentation not bound", nslots() == 0);

    clu_mysensors_cfg_t c = s_base;
    c.max_modules = 3;
    s_endpoints = 0;
    clu_mysensors_init(&c);
    uint8_t hw = 0;
    check("init: bus endpoint installed, no module before a sensor needs one",
          s_endpoints == 1 && s_endpoint == ms_vmod_frame && !nvs_peek("mysensors", "vsn") && discover(&hw) == 0);

    // each node gets its own module
    feed("12;1;0;0;0;Door\n12;2;0;0;3;Relay\n12;3;0;0;6;Temp\n12;1;0;0;0;Door\n13;1;0;0;19;Lock\n");
    const nvs_entry_t *e = nvs_peek("mysensors", "vsn");
    uint32_t sn[MS_VMOD_MAX] = {0};
    if (e) {
        memcpy(sn, e->val, sizeof sn);
    }
    const nvs_entry_t *own = nvs_peek("mysensors", "vown");
    check("node 12 -> module 1, node 13 -> module 2 (serials and owners saved)",
          e && sn[0] && sn[1] && sn[0] != sn[1] && sn[0] < 0x80000000u && !sn[2] && own && own->val[0] == 12 &&
              own->val[1] == 13);
    ms_vmod_slot_t v[64];
    int n = ms_vmod_list(v, 64);
    check("door -> DIN 1, relay -> DOUT 1 (module 1), lock -> DOUT 1 (module 2), temperature not bound, no duplicates",
          n == 3 && at(12, 1, 0, MS_SLOT_DIN, 0) && at(12, 2, 0, MS_SLOT_DOUT, 0) && at(13, 1, 1, MS_SLOT_DOUT, 0) &&
              v[0].vtype == 16 && v[0].sn == sn[0] && v[0].hw_type == 0x1e && v[2].vtype == 36);
    feed("0;1;0;0;3;GW\n");
    check("gateway / id-less nodes: not bound", nslots() == 3);

    // discovery / addressing like the bus master does it
    check("DISCOVER: module 1, IO_MODULE_DIN_8", discover(&hw) == sn[0] && hw == 0x1e);
    check("ADDR_ASSIGN: ACK", is_ack(bus(tfbus_addr_assign(f, sizeof f, sn[0], 5))));
    check("DISCOVER: module 2", discover(&hw) == sn[1]);
    bus(tfbus_addr_assign(f, sizeof f, sn[1], 6));
    check("DISCOVER: all addressed -> no reply", discover(&hw) == 0);
    check("ADDR_ASSIGN for another serial: not ours", bus(tfbus_addr_assign(f, sizeof f, 1234, 7)) == 0);
    n = bus(tfbus_version_query(f, sizeof f, 5));
    check("VERSION_QUERY: fwApiVer 2, 2.4, hw 3", n == 8 && frx[3] == 2 && frx[4] == 2 && frx[5] == 4 && frx[6] == 3);
    check("other address: not ours", bus(tfbus_status_poll(f, sizeof f, 9)) == 0);

    // status follows the nodes
    uint8_t in = 0xff, out = 0xff;
    check("STATUS_POLL: nothing reported yet", poll(5, &in, &out) && in == 0 && out == 0);
    feed("12;1;1;0;16;1\n12;2;1;0;2;1\n12;1;1;0;0;1\n");
    check("STATUS_POLL: tripped door -> DIN 1, relay on -> DOUT 1, other value types ignored",
          poll(5, &in, &out) && in == 0x01 && out == 0x01);

    // outputs from OM go to the nodes
    lua_flush();
    check("MODULE_CMD DEACTIVATE: ACK", is_ack(module_cmd(5, 0x09, 0x01)));
    tick(0);
    check("DOUT 1 off -> C_SET V_STATUS 0 to node 12/2", sent("12;2;1;0;2;0\n"));
    module_cmd(5, 0x08, 0x07);
    tick(1);
    check("ACTIVATE 3 outputs: the bound one sent, unbound kept", sent("12;2;1;0;2;1\n") && poll(5, &in, &out) && out == 0x07);
    module_cmd(6, 0x08, 0x01);
    tick(2);
    check("module 2 DOUT 1 -> node 13's lock", sent("13;1;1;0;36;1\n"));
    module_cmd(5, 0x08, 0x01);
    tick(3);
    check("no change -> nothing sent", sent(""));
    ms_msg_t m = {.node = 12, .child = 2, .cmd = MS_C_SET, .type = 2, .payload = "0"};
    clu_mysensors_send(&m);
    tick(4);
    drain();
    check("script switches the relay: DOUT follows", poll(5, &in, &out) && out == 0x06);
    feed("13;1;1;0;36;0\n");
    check("node reports its lock open: DOUT follows", poll(6, &in, &out) && out == 0x00);
    check("class frame: ACK", is_ack(bus(tfbus_cmd(f, sizeof f, 5, 0x00, NULL, 0))));
    const uint8_t badpl[1] = {0};
    check("malformed MODULE_CMD: no reply", bus(tfbus_build(f, sizeof f, 5, TFBUS_T_MODULE_CMD, badpl, 1)) < 0);
    while (clu_mysensors_send(&m)) {
    }
    check("outputs with the queue full: ACKed, warning", is_ack(module_cmd(5, 0x08, 0x01)));
    while (s_txq->n) {
        tick(5);
        drain();
    }
    module_cmd(5, 0x09, 0xff); // all off
    tick(6);
    drain();

    // channels follow the child ids; a lower id presented late moves the others, states with them
    feed("12;5;0;0;3;R5\n12;5;1;0;2;1\n");
    check("child 5 after child 2 -> DOUT 2, on", at(12, 5, 0, MS_SLOT_DOUT, 1) && poll(5, &in, &out) && out == 0x02);
    feed("12;3;0;0;3;R3\n");
    check("child 3 presented later: DOUT 2, child 5 moved to DOUT 3 with its state",
          at(12, 2, 0, MS_SLOT_DOUT, 0) && at(12, 3, 0, MS_SLOT_DOUT, 1) && at(12, 5, 0, MS_SLOT_DOUT, 2) &&
              poll(5, &in, &out) && out == 0x04);

    // more than 8 outputs: the node's second module
    for (int i = 10; i <= 16; i++) {
        char l[48];
        snprintf(l, sizeof l, "12;%d;0;0;3;R%d\n", i, i);
        feed(l);
    }
    e = nvs_peek("mysensors", "vsn");
    memcpy(sn, e->val, sizeof sn);
    check("node 12: 10 outputs -> DOUT 1-8 on module 1, the rest on its own module 3",
          at(12, 14, 0, MS_SLOT_DOUT, 7) && at(12, 15, 2, MS_SLOT_DOUT, 0) && at(12, 16, 2, MS_SLOT_DOUT, 1) &&
              sn[2] && sn[2] != sn[0] && sn[2] != sn[1]);
    check("DISCOVER: the new module 3 answers", discover(&hw) == sn[2]);
    bus(tfbus_addr_assign(f, sizeof f, sn[2], 7));
    feed("20;1;0;0;1;Motion\n");
    check("node 20: limit (3 modules) reached -> not bound", nslots() == 1 + 10 + 1 && !at(20, 1, 0, MS_SLOT_DIN, 0));
    n = ms_vmod_list(v, 2);
    check("list: capped to the buffer", n == 2);

    // unmap frees node 13's module; node 20 gets it (same serial)
    check("unmap node 13: its module emptied", ms_vmod_unmap(13, 255) == 1);
    feed("20;1;0;0;1;Motion\n");
    check("node 20 reuses the freed module 2", at(20, 1, 1, MS_SLOT_DIN, 0));

    // reboot: same serials, channels and owners
    clu_mysensors_init(&c);
    n = ms_vmod_list(v, 64);
    check("reboot: serials, bindings, owners from NVS",
          n == 12 && v[0].sn == sn[0] && at(12, 16, 2, MS_SLOT_DOUT, 1) && at(20, 1, 1, MS_SLOT_DIN, 0) &&
              ms_vmod_owner(0) == 12 && ms_vmod_owner(1) == 20 && ms_vmod_owner(2) == 12 && ms_vmod_owner(3) == -1);

    // RESET: unaddressed again
    discover(&hw);
    bus(tfbus_addr_assign(f, sizeof f, sn[0], 5));
    check("RESET to the module: no reply, unaddressed", bus(tfbus_reset(f, sizeof f, 5)) < 0 && discover(&hw) == sn[0]);
    check("RESET broadcast: also for the wire", bus(tfbus_reset(f, sizeof f, TFBUS_BROADCAST)) == 0);
    check("SEEK: for the wire", bus(tfbus_seek(f, sizeof f)) == 0);

    // unmap
    check("unmap one child", ms_vmod_unmap(12, 16) == 1 && nslots() == 11);
    check("unmap unknown: nothing", ms_vmod_unmap(99, 255) == 0);

    // Lua
    L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    luaL_setfuncs(L, s_fns, 0);
    lua_setglobal(L, "__clu");
    check("lua: ms_slots", run("local t = __clu.ms_slots(); local s = t[1]; return #t == 11 and s.module == 1 and "
                               "s.kind == 'din' and s.channel == 1 and s.node == 12 and s.child == 1 and s.vtype == 16 "
                               "and s.sn > 0 and t[2].kind == 'dout'"));
    check("lua: ms_unmap", run("return __clu.ms_unmap(20) == 1 and __clu.ms_unmap(12, 2) == 1 and #__clu.ms_slots() == 9"));
    lua_close(L);

    // NVS from before owners were kept: a one-node module is that node's, a mixed one nobody's
    {
        nvs_handle_t h;
        nvs_open("mysensors", NVS_READWRITE, &h);
        uint32_t old_sn[4] = {0x01020304, 0x05060708};
        uint8_t old_map[4][MS_VMOD_SLOTS][4] = {{{1, 40, 2, 16}}, {{1, 41, 1, 16}, {1, 42, 1, 16}}};
        nvs_set_blob(h, "vsn", old_sn, sizeof old_sn);
        nvs_set_blob(h, "vmap", old_map, sizeof old_map);
        nvs_entry_t *o = (nvs_entry_t *)nvs_peek("mysensors", "vown");
        o->len = 0; // the key as absent (length 0 is refused)
        clu_mysensors_init(&c);
        check("old NVS layout: serials and bindings loaded, owner derived",
              nslots() == 3 && ms_vmod_owner(0) == 40 && ms_vmod_owner(1) == 0 && at(40, 2, 0, MS_SLOT_DIN, 0));
        feed("40;1;0;0;0;Door\n41;9;0;0;0;Door\n");
        check("old layout: node 40 sorts into its module, node 41 gets a new one (mixed one not taken)",
              at(40, 1, 0, MS_SLOT_DIN, 0) && at(40, 2, 0, MS_SLOT_DIN, 1) && at(41, 9, 2, MS_SLOT_DIN, 0) &&
                  at(41, 1, 1, MS_SLOT_DIN, 0));
        nvs_set_blob(h, "vsn", old_sn, 6);
        clu_mysensors_init(&c);
        check("NVS blob of a bad size: ignored", discover(&hw) == 0);
    }

    // NVS unavailable: modules still created, nothing kept
    nvs_fail_open = 1;
    clu_mysensors_init(&c);
    feed("30;1;0;0;0;Door\n");
    nvs_fail_open = 0;
    check("NVS unavailable: modules still work", discover(&hw) != 0 && at(30, 1, 0, MS_SLOT_DIN, 0));

    // workaround: S_BINARY described "INPUT..." is a binary input (relay/input boards)
    nvs_fail_open = 1;
    c.max_modules = 4;
    clu_mysensors_init(&c);
    feed("60;6;0;0;3;RELAY 1\n60;0;0;0;3;Input 1\n60;1;0;0;3;INPUT 2\n60;7;0;0;3;RELAY 2\n");
    check("INPUT workaround: inputs on DIN, relays on DOUT, one module",
          at(60, 0, 0, MS_SLOT_DIN, 0) && at(60, 1, 0, MS_SLOT_DIN, 1) && at(60, 6, 0, MS_SLOT_DOUT, 0) &&
              at(60, 7, 0, MS_SLOT_DOUT, 1) && nslots() == 4);
    uint32_t sn60 = discover(&hw);
    bus(tfbus_addr_assign(f, sizeof f, sn60, 5));
    feed("60;1;1;0;2;1\n60;7;1;0;2;1\n");
    check("INPUT workaround: V_STATUS drives the DIN", poll(5, &in, &out) && in == 0x02 && out == 0x02);
    for (int i = 0; i <= 8; i++) {
        char l[48];
        snprintf(l, sizeof l, "61;%d;0;0;3;R%d\n", i, i);
        feed(l);
    }
    check("node 61: 9 outputs -> modules 2 and 3", at(61, 7, 1, MS_SLOT_DOUT, 7) && at(61, 8, 2, MS_SLOT_DOUT, 0));
    feed("61;0;0;0;3;INPUT 1\n");
    check("re-presented as INPUT: moved to DIN, outputs close up, spare module released",
          at(61, 0, 1, MS_SLOT_DIN, 0) && at(61, 1, 1, MS_SLOT_DOUT, 0) && at(61, 8, 1, MS_SLOT_DOUT, 7) &&
              nslots() == 4 + 9 && ms_vmod_owner(2) == 0);
    feed("62;1;0;0;0;Door\n");
    check("released module reused by the next node", at(62, 1, 2, MS_SLOT_DIN, 0) && ms_vmod_owner(2) == 62);

    // discovery: the node check runs then; modules of nodes that do not answer are removed
    reset_state();
    s_now_us = 10000 * 1000;
    clu_mysensors_init(&c); // check at start (ids: none, NVS unavailable)
    feed("70;1;0;0;0;Door\n71;1;0;0;3;Relay\n72;1;0;0;0;Door\n72;1;1;0;16;1\n");
    n = ms_vmod_list(v, 64);
    uint32_t sn72 = n == 3 ? v[2].sn : 0;
    drain();
    for (int64_t t = 10000; t <= 14000; t += 100) {
        tick(t);
    }
    check("check at start: every known id asked to present itself, 0.5 s for a silent one",
          sent("70;255;3;0;19;0\n71;255;3;0;19;0\n72;255;3;0;19;0\n"));
    memset(s_answer, 0, sizeof s_answer);
    s_answer[70] = s_answer[71] = s_answer[72] = true;
    s_now_us = 20000 * 1000;
    clu_mysensors_refresh();
    check("discovery: all answered, nothing removed; next node asked as soon as one answers",
          nslots() == 3 && !probe_running() && s_now_us < 20200 * 1000 && sent("70;255;3;0;19;0\n71;255;3;0;19;0\n72;255;3;0;19;0\n"));
    s_answer[71] = false; // 71 gone, though heard just before the discovery (e.g. back under a new id)
    s_now_us = 29900 * 1000;
    feed("71;1;1;0;2;0\n");
    s_now_us = 30000 * 1000;
    clu_mysensors_refresh();
    n = ms_vmod_list(v, 64);
    check("discovery: node 71 and its module removed, node 72's module moved down (same SN)",
          n == 2 && at(70, 1, 0, MS_SLOT_DIN, 0) && at(72, 1, 1, MS_SLOT_DIN, 0) && v[1].sn == sn72 &&
              ms_vmod_owner(1) == 72 && ms_vmod_owner(2) == -1);
    check("discovery: a silent node costs 0.5 s", s_now_us >= 30500 * 1000 && s_now_us < 30800 * 1000);
    discover(&hw);
    bus(tfbus_addr_assign(f, sizeof f, v[0].sn, 5));
    check("moved module: answers DISCOVER with its serial", discover(&hw) == sn72);
    bus(tfbus_addr_assign(f, sizeof f, sn72, 6));
    check("moved module: state kept", poll(6, &in, &out) && in == 0x01);
    check("node 71 forgotten (id, registry, children, values); 70 and 72 kept",
          !id_taken(71) && !node_find(71) && !value_find(71, 1, 2) && id_taken(70) && node_find(70) &&
              id_taken(72) && node_find(72));
    int kids71 = 0;
    for (int i = 0; i < MS_MAX_CHILDREN; i++) {
        kids71 += s_children[i].used && s_children[i].node == 71;
    }
    check("node 71's children forgotten", kids71 == 0);
    drain();
    clu_mysensors_refresh();
    check("node 71's id freed: the next check does not ask it", sent("70;255;3;0;19;0\n72;255;3;0;19;0\n"));
    memset(s_answer, 0, sizeof s_answer);
    drain();
    s_uart_stuck = true; // nobody asked
    int64_t t0 = s_now_us;
    clu_mysensors_refresh();
    s_uart_stuck = false;
    check("check not finished in time (2 nodes: 2.2 s): nothing removed",
          nslots() == 2 && s_now_us - t0 >= 2200 * 1000 && s_now_us - t0 < 2300 * 1000);
    reset_state();
    nvs_fail_open = 0;
    lua_flush();
    s_out[0] = 0;
    s_now_us = 0;
}

int main(void)
{
    test_proto();
    test_init();
    test_ids();
    test_internal();
    test_values();
    test_tables();
    test_flow();
    test_task();
    test_lua();
    test_rs485_proto();
    test_gateway();
    test_vmod();
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
