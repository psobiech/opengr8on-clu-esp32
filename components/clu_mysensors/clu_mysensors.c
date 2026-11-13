/**
 * @file clu_mysensors.c
 * @brief MySensors gateway on the RS485 network: frames, node ids, value cache, Lua bindings.
 */
#include "clu_mysensors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clu_lua.h"
#include "ms_rs485.h"
#include "ms_vmod.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "mysensors";

extern const char mysensors_lua_start[] asm("_binary_mysensors_lua_start");
extern const char mysensors_lua_end[] asm("_binary_mysensors_lua_end");

/** @brief Nodes kept in the registry (least recently seen dropped when full). */
#define MS_MAX_NODES 32
/** @brief Presented child sensors kept. */
#define MS_MAX_CHILDREN 128
/** @brief Values kept for C_REQ answers / MySensors.get() (oldest dropped when full). */
#define MS_MAX_VALUES 128
/** @brief Messages waiting for the scripts / for the wire. */
#define MS_LUA_QUEUE 32
#define MS_TX_QUEUE 16

#define NVS_NS "mysensors"
#define NVS_IDS "ids"

/** @brief Node seen on the network. */
typedef struct {
    bool used;
    uint8_t id;                         /**< 0 = the gateway, 1..254 = nodes. */
    int8_t battery;                     /**< I_BATTERY_LEVEL, -1 = unknown. */
    char sketch[MS_MAX_PAYLOAD + 1];    /**< I_SKETCH_NAME. */
    char version[MS_MAX_PAYLOAD + 1];   /**< I_SKETCH_VERSION. */
    char lib[MS_MAX_PAYLOAD + 1];       /**< Library version (presentation of child 255). */
    uint8_t type;                       /**< Node presentation type (S_ARDUINO_NODE / _REPEATER_NODE). */
    int64_t last_ms;                    /**< Last message from it. */
} ms_node_t;

/** @brief Presented child sensor. */
typedef struct {
    bool used;
    uint8_t node, child, type;
    char desc[MS_MAX_PAYLOAD + 1];
} ms_child_t;

/** @brief Last value of node/child/V_ type. */
typedef struct {
    bool used;
    uint8_t node, child, type;
    char payload[MS_MAX_PAYLOAD + 1];
    int64_t ms;
} ms_value_t;

static clu_mysensors_cfg_t s_cfg;
static SemaphoreHandle_t s_mx;            // registry, cache, ids, stats
static QueueHandle_t s_luaq, s_txq;
static ms_node_t s_nodes[MS_MAX_NODES];
static ms_child_t s_children[MS_MAX_CHILDREN];
static ms_value_t s_values[MS_MAX_VALUES];
static uint8_t s_ids[32];                 // bit n: node id n handed out / seen
static char s_gw_version[MS_MAX_PAYLOAD + 1];
static uint32_t s_rx, s_tx, s_dropped, s_bad;
/** @brief How the bus looks (judged every GW_CHECK_MS when something came in). */
typedef enum {
    LINK_UNKNOWN,   /**< Nothing received yet. */
    LINK_GARBAGE,   /**< Bytes, but no valid frame: baud rate, A/B swapped. */
    LINK_OK,        /**< Valid frames from the nodes. */
} link_t;
static const char *const LINK_NAME[] = {"unknown", "garbage", "ok"};
static link_t s_link;
static int64_t s_check_ms;                   // next link check

static struct {
    uint32_t bytes, frames, bad;
    uint8_t raw[32];   /**< First bytes of the window, shown when the link looks like garbage. */
} s_win;                                     // since the last check
static uint32_t s_rx_bytes;
static bool s_trace;                         // every message at INFO (MySensors.trace)
// the CLU is node 0, the gateway, on the MY_RS485 bus
#define GW_QUEUE 16          // frames waiting for a quiet bus
#define GW_IDLE_MS 5         // bus quiet this long before we send (nodes wait for silence too)
#define GW_CHECK_MS 30000    // link judged this often (nodes talk only when something happens)
static ms_rs485_rx_t s_frx;
static struct {
    uint8_t len;
    uint8_t buf[MS_RS485_MAX_FRAME];
} s_gwq[GW_QUEUE];
static int s_gwq_head, s_gwq_n;
static int64_t s_last_byte_ms = -1000;

// Node check: every known node id is asked to present itself (I_PRESENTATION), one at a time: the
// next one as soon as the node answers (its first frame comes within ~50 ms), else after
// PROBE_WAIT_MS. Run at start (the nodes' presentations fill the registry) and by every discovery,
// which waits for it and removes the nodes that did not answer (clu_mysensors_refresh). State
// under s_mx.
#define PROBE_DELAY_MS 2000    // check at start this long after it
#define PROBE_WAIT_MS 500      // a node's answer awaited this long
#define PROBE_POLL_MS 20       // discovery: check progress looked at this often
static int s_probe_next = 256; // next id to ask; 256 = all asked
static int s_probe_id;         // id whose answer is awaited (0 = none)
static int64_t s_probe_since;  // check began
static int64_t s_probe_at;     // first request not before
static int64_t s_probe_sent;   // request to s_probe_id sent

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// ---------------------------------------------------------------------------------------------
// Node ids (NVS)
// ---------------------------------------------------------------------------------------------
static bool id_taken(uint8_t id)
{
    return s_ids[id / 8] & (1u << (id % 8));
}

static void ids_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: node ids not saved");
        return;
    }
    nvs_set_blob(h, NVS_IDS, s_ids, sizeof s_ids);
    nvs_commit(h);
    nvs_close(h);
}

static void ids_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof s_ids;
    memset(s_ids, 0, sizeof s_ids);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, NVS_IDS, s_ids, &len) != ESP_OK || len != sizeof s_ids) {
            memset(s_ids, 0, sizeof s_ids);
        }
        nvs_close(h);
    }
}

/** @brief Mark a node id used (a node may come with a fixed id): saved when new. Lock held. */
static void id_mark(uint8_t id)
{
    if (id == MS_GATEWAY || id == MS_BROADCAST || id_taken(id)) {
        return;
    }
    s_ids[id / 8] |= (uint8_t)(1u << (id % 8));
    ids_save();
}

/**
 * @brief Free the ids of the nodes not in @p alive (saved) and forget what they reported (node,
 * children, values), so the node check stops asking them and I_ID_REQUEST may hand them out again.
 * Lock held.
 */
static void ids_forget(const uint8_t alive[32])
{
    bool freed = false;
    for (int id = 1; id < MS_BROADCAST; id++) {
        const uint8_t bit = (uint8_t)(1u << (id % 8));
        if (!id_taken((uint8_t)id) || (alive[id / 8] & bit)) {
            continue;
        }
        ESP_LOGW(TAG, "node %d not heard: id freed", id);
        s_ids[id / 8] &= (uint8_t)~bit;
        freed = true;
        for (int i = 0; i < MS_MAX_NODES; i++) {
            if (s_nodes[i].used && s_nodes[i].id == id) {
                s_nodes[i].used = false;
            }
        }
        for (int i = 0; i < MS_MAX_CHILDREN; i++) {
            if (s_children[i].used && s_children[i].node == id) {
                s_children[i].used = false;
            }
        }
        for (int i = 0; i < MS_MAX_VALUES; i++) {
            if (s_values[i].used && s_values[i].node == id) {
                s_values[i].used = false;
            }
        }
    }
    if (freed) {
        ids_save();
    }
}

/** @brief Lowest free node id, marked used; 0 if all 254 are taken. Lock held. */
static uint8_t id_alloc(void)
{
    for (int id = 1; id < MS_BROADCAST; id++) {
        if (!id_taken((uint8_t)id)) {
            id_mark((uint8_t)id);
            return (uint8_t)id;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Registry and value cache (lock held)
// ---------------------------------------------------------------------------------------------
static ms_node_t *node_find(uint8_t id)
{
    for (int i = 0; i < MS_MAX_NODES; i++) {
        if (s_nodes[i].used && s_nodes[i].id == id) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

/** @brief Node entry, created (dropping the least recently seen one when full). */
static ms_node_t *node_get(uint8_t id, int64_t now)
{
    ms_node_t *n = node_find(id);
    if (n) {
        return n;
    }
    n = &s_nodes[0];
    for (int i = 0; i < MS_MAX_NODES; i++) {
        if (!s_nodes[i].used) {
            n = &s_nodes[i];
            break;
        }
        if (s_nodes[i].last_ms < n->last_ms) {
            n = &s_nodes[i];
        }
    }
    memset(n, 0, sizeof *n);
    n->used = true;
    n->id = id;
    n->battery = -1;
    n->last_ms = now;
    return n;
}

static void child_set(uint8_t node, uint8_t child, uint8_t type, const char *desc)
{
    ms_child_t *free_slot = NULL;
    for (int i = 0; i < MS_MAX_CHILDREN; i++) {
        ms_child_t *c = &s_children[i];
        if (c->used && c->node == node && c->child == child) {
            free_slot = c;
            break;
        }
        if (!c->used && !free_slot) {
            free_slot = c;
        }
    }
    if (!free_slot) {
        ESP_LOGW(TAG, "child table full: %u/%u not kept", node, child);
        return;
    }
    free_slot->used = true;
    free_slot->node = node;
    free_slot->child = child;
    free_slot->type = type;
    strlcpy(free_slot->desc, desc, sizeof free_slot->desc);
}

static ms_value_t *value_find(uint8_t node, uint8_t child, uint8_t type)
{
    for (int i = 0; i < MS_MAX_VALUES; i++) {
        ms_value_t *v = &s_values[i];
        if (v->used && v->node == node && v->child == child && v->type == type) {
            return v;
        }
    }
    return NULL;
}

static void value_set(uint8_t node, uint8_t child, uint8_t type, const char *payload, int64_t now)
{
    ms_value_t *v = value_find(node, child, type);
    if (!v) {
        v = &s_values[0];
        for (int i = 0; i < MS_MAX_VALUES; i++) {
            if (!s_values[i].used) {
                v = &s_values[i];
                break;
            }
            if (s_values[i].ms < v->ms) {
                v = &s_values[i];
            }
        }
        v->used = true;
        v->node = node;
        v->child = child;
        v->type = type;
    }
    strlcpy(v->payload, payload, sizeof v->payload);
    v->ms = now;
}

// ---------------------------------------------------------------------------------------------
// Wire
// ---------------------------------------------------------------------------------------------
static void trace_line(const char *dir, const ms_msg_t *m, const char *note)
{
    char line[MS_LINE_CAP];
    if (ms_format(m, line, sizeof line) <= 0) {
        return;
    }
    line[strlen(line) - 1] = 0;
    if (s_trace) {
        ESP_LOGI(TAG, "%s %s%s", dir, line, note);
    } else {
        ESP_LOGD(TAG, "%s %s%s", dir, line, note);
    }
}

/** @brief Queue a frame (MyMessage @p data) for node @p to until the bus is quiet. */
static void gw_queue(uint8_t to, const uint8_t *data, size_t len)
{
    if (s_gwq_n == GW_QUEUE) {
        s_dropped++;
        ESP_LOGW(TAG, "RS485 send queue full: frame to %u dropped", to);
        return;
    }
    int i = (s_gwq_head + s_gwq_n++) % GW_QUEUE;
    s_gwq[i].len = (uint8_t)ms_rs485_encode(to, MS_GATEWAY, data, len, s_gwq[i].buf, sizeof s_gwq[i].buf);
}

/** @brief Send queued frames if nobody talked for GW_IDLE_MS (UART task). */
static void gw_flush(int64_t now)
{
    while (s_gwq_n && now - s_last_byte_ms >= GW_IDLE_MS) {
        int i = s_gwq_head;
        s_gwq_head = (s_gwq_head + 1) % GW_QUEUE;
        s_gwq_n--;
        if (uart_write_bytes(s_cfg.uart_num, s_gwq[i].buf, s_gwq[i].len) != s_gwq[i].len) {
            ESP_LOGW(TAG, "UART write failed");
            continue;
        }
        if (s_cfg.de_pin >= 0) {
            uart_wait_tx_done(s_cfg.uart_num, pdMS_TO_TICKS(100));
        }
        s_tx++;
    }
}

/** @brief A message (controller side, node = destination) to a node: frame queued (UART task only). */
static void wire(const ms_msg_t *m)
{
    uint8_t bin[MS_RS485_MAX_DATA];
    size_t n = m->node == MS_GATEWAY ? 0 : ms_bin_encode(m, MS_GATEWAY, bin, sizeof bin);
    if (!n) {
        ESP_LOGW(TAG, "message to %u/%u not sent (to the gateway itself / too long)", m->node, m->child);
        return;
    }
    trace_line(">", m, "");
    gw_queue(m->node, bin, n);
}

static void reply(uint8_t node, uint8_t child, uint8_t cmd, uint8_t type, const char *payload)
{
    ms_msg_t r = {.node = node, .child = child, .cmd = cmd, .type = type};
    strlcpy(r.payload, payload, sizeof r.payload);
    wire(&r);
}

/** @brief Local time as seconds since 1970 (what MySensors nodes expect); 0 if the clock is unset. */
static long long local_epoch(void)
{
    time_t t = time(NULL);
    struct tm lt;
    if (t < 1609459200 || !localtime_r(&t, &lt)) { // before 2021: clock not set yet
        return 0;
    }
    // days from civil (proleptic Gregorian), no dependency on tm_gmtoff / timegm
    int y = lt.tm_year + 1900, m = lt.tm_mon + 1;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + lt.tm_mday - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = (long long)era * 146097 + doe - 719468;
    return days * 86400 + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
}

// ---------------------------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------------------------
/** @brief C_INTERNAL from the network. Lock held. Returns false if the scripts need not see it. */
static bool on_internal(const ms_msg_t *m, int64_t now)
{
    char buf[24];
    switch (m->type) {
    case MS_I_ID_REQUEST: {
        uint8_t id = id_alloc();
        if (!id) {
            ESP_LOGE(TAG, "I_ID_REQUEST: all node ids taken");
            break;
        }
        ESP_LOGI(TAG, "new node id %u", id);
        snprintf(buf, sizeof buf, "%u", id);
        // child = the request's token (MySensors 2.x AUTO nodes send a random one and take a
        // response carrying it or 255): echoed, so a second node asking at the same time keeps
        // waiting for its own instead of taking this id too
        reply(MS_BROADCAST, m->child, MS_C_INTERNAL, MS_I_ID_RESPONSE, buf);
        break;
    }
    case MS_I_CONFIG:
        reply(m->node, MS_BROADCAST, MS_C_INTERNAL, MS_I_CONFIG, s_cfg.metric ? "M" : "I");
        break;
    case MS_I_TIME: {
        long long t = local_epoch();
        if (t) {
            snprintf(buf, sizeof buf, "%lld", t);
            reply(m->node, MS_BROADCAST, MS_C_INTERNAL, MS_I_TIME, buf);
        }
        break;
    }
    case MS_I_LOG_MESSAGE:
        ESP_LOGD(TAG, "node %u: %s", m->node, m->payload);
        return false;
    case MS_I_SKETCH_NAME:
    case MS_I_SKETCH_VERSION:
    case MS_I_BATTERY_LEVEL:
        if (m->node != MS_BROADCAST) {
            ms_node_t *n = node_get(m->node, now);
            if (m->type == MS_I_SKETCH_NAME) {
                strlcpy(n->sketch, m->payload, sizeof n->sketch);
            } else if (m->type == MS_I_SKETCH_VERSION) {
                strlcpy(n->version, m->payload, sizeof n->version);
            } else {
                int b = atoi(m->payload);
                n->battery = (int8_t)(b < 0 ? 0 : b > 100 ? 100 : b);
            }
        }
        break;
    default:
        break;
    }
    return true;
}

/** @brief A message from a node, for the controller. */
static void handle(const ms_msg_t *m, int64_t now)
{
    bool to_lua = true;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_rx++;
    if (m->node != MS_BROADCAST) {
        id_mark(m->node);
        node_get(m->node, now)->last_ms = now;
    }
    switch (m->cmd) {
    case MS_C_PRESENTATION:
        if (m->child == MS_BROADCAST) {
            if (m->node != MS_BROADCAST) {
                ms_node_t *n = node_get(m->node, now);
                n->type = m->type;
                strlcpy(n->lib, m->payload, sizeof n->lib);
            }
        } else {
            child_set(m->node, m->child, m->type, m->payload);
            ms_vmod_present(m->node, m->child, m->type, m->payload);
        }
        break;
    case MS_C_SET:
        value_set(m->node, m->child, m->type, m->payload, now);
        ms_vmod_value(m->node, m->child, m->type, m->payload);
        break;
    case MS_C_REQ: {
        const ms_value_t *v = value_find(m->node, m->child, m->type);
        if (v) {
            reply(m->node, m->child, MS_C_SET, m->type, v->payload);
        }
        break;
    }
    case MS_C_INTERNAL:
        to_lua = on_internal(m, now);
        break;
    default: // C_STREAM: no firmware updates from here
        break;
    }
    xSemaphoreGive(s_mx);
    if (to_lua && xQueueSend(s_luaq, m, 0) != pdTRUE) {
        s_dropped++;
    }
}

/** @brief A gateway's own answer (one byte, P_BYTE) to node @p to. */
static void gw_answer(uint8_t to, uint8_t type, uint8_t value)
{
    uint8_t bin[MS_BIN_HEADER + 1];
    size_t n = ms_bin_encode_byte(MS_GATEWAY, to, MS_BROADCAST, MS_C_INTERNAL, type, value, bin, sizeof bin);
    gw_queue(to, bin, n);
}

/**
 * @brief A frame for us (node 0) or everyone. What a MySensors gateway answers on its own (find
 * parent, ping, registration, echo) is answered here, the rest goes to the controller.
 */
static void gw_frame(const ms_rs485_frame_t *f, int64_t now)
{
    ms_bin_t b;
    if (!ms_bin_decode(f->data, f->len, &b)) {
        s_bad++;
        s_win.bad++;
        ESP_LOGW(TAG, "RS485 frame from %u: not a MySensors 2.x message", f->from);
        return;
    }
    s_win.frames++;
    trace_line("<", &b.m, f->to == MS_BROADCAST ? " (broadcast)" : "");
    const uint8_t cmd = b.m.cmd, type = b.m.type;
    if (f->to == MS_BROADCAST) {
        if (cmd != MS_C_INTERNAL) {
            handle(&b.m, now);
        } else if (type == MS_I_FIND_PARENT_REQUEST) {
            gw_answer(b.sender, MS_I_FIND_PARENT_RESPONSE, 0); // distance to the gateway: 0
        }
        return; // other internal broadcasts are for the nodes
    }
    if (b.req_echo && !b.is_echo) { // the sender wants its message back, flagged as echo
        uint8_t e[MS_RS485_MAX_DATA];
        memcpy(e, f->data, f->len);
        e[0] = MS_GATEWAY;
        e[1] = MS_GATEWAY;
        e[2] = b.sender;
        e[4] = (uint8_t)((e[4] & ~0x08) | 0x10);
        gw_queue(b.sender, e, f->len);
    }
    if (!b.is_echo && cmd == MS_C_INTERNAL) {
        switch (type) {
        case MS_I_PING:
            gw_answer(b.sender, MS_I_PONG, 1);
            return;
        case MS_I_REGISTRATION_REQUEST:
            gw_answer(b.sender, MS_I_REGISTRATION_RESPONSE, 1); // approved, like a gateway without
            return;                                              // MY_REGISTRATION_CONTROLLER
        case MS_I_PONG:
        case MS_I_SIGNAL_REPORT_REVERSE:
            return;
        default:
            break;
        }
    }
    handle(&b.m, now);
}

/** @brief Bytes from the bus through the frame receiver. */
static void gw_bytes(const uint8_t *b, size_t len, int64_t now)
{
    s_last_byte_ms = now;
    for (size_t i = 0; i < len; i++) {
        if (s_win.bytes + i < sizeof s_win.raw) {
            s_win.raw[s_win.bytes + i] = b[i];
        }
        ms_rs485_frame_t f;
        int r = ms_rs485_rx_push(&s_frx, b[i], &f);
        if (r > 0) {
            gw_frame(&f, now);
        } else if (r < 0) {
            s_bad++;
            s_win.bad++;
        }
    }
    s_rx_bytes += (uint32_t)len;
    s_win.bytes += (uint32_t)len;
}

/** @brief Judge the bus from what came in since the last check (only called when bytes did); log changes. */
static void check_link(void)
{
    link_t l = s_win.frames ? LINK_OK : LINK_GARBAGE; // bytes, never a valid frame
    if (l == s_link) {
        return;
    }
    s_link = l;
    if (l == LINK_OK) {
        ESP_LOGI(TAG, "RS485 network: valid frames from the nodes");
    } else {
        // the bytes tell: random ones = baud rate, 00 / ff / framing noise = RS485 wiring / bias
        char hex[3 * sizeof s_win.raw + 1], txt[sizeof s_win.raw + 1];
        size_t n = s_win.bytes < sizeof s_win.raw ? s_win.bytes : sizeof s_win.raw;
        for (size_t i = 0; i < n; i++) {
            snprintf(hex + 3 * i, 4, "%02x ", s_win.raw[i]);
            txt[i] = s_win.raw[i] >= 0x20 && s_win.raw[i] < 0x7f ? (char)s_win.raw[i] : '.';
        }
        hex[n ? 3 * n - 1 : 0] = 0;
        txt[n] = 0;
        ESP_LOGW(TAG, "RS485 network: %lu byte(s), %lu bad frame(s), no valid one: baud rate (%d) or RS485 "
                      "A/B swapped / no bias?",
                 (unsigned long)s_win.bytes, (unsigned long)s_win.bad, s_cfg.baud);
        ESP_LOGW(TAG, "  first bytes: %s  |%s|", hex, txt);
    }
}

/** @brief Node check: the awaited answer judged, the next known node id asked when due (UART task). */
static void probe(int64_t now)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_probe_id) {
        const ms_node_t *n = node_find((uint8_t)s_probe_id);
        if ((n && n->last_ms >= s_probe_sent) || now - s_probe_sent >= PROBE_WAIT_MS) {
            s_probe_id = 0; // answered / not answering: next one
        }
    }
    if (!s_probe_id && s_probe_next <= 254 && now >= s_probe_at && s_gwq_n < GW_QUEUE) {
        int id = s_probe_next;
        while (id <= 254 && !id_taken((uint8_t)id)) {
            id++;
        }
        if (id <= 254) {
            reply((uint8_t)id, MS_BROADCAST, MS_C_INTERNAL, MS_I_PRESENTATION, "");
            s_probe_id = id;
            s_probe_sent = now;
        }
        s_probe_next = id < 254 ? id + 1 : 256;
    }
    xSemaphoreGive(s_mx);
}

/** @brief Start a node check (lock held). */
static void probe_start(int64_t now, int64_t delay)
{
    s_probe_next = 1;
    s_probe_id = 0;
    s_probe_since = now;
    s_probe_at = now + delay;
}

static bool probe_running(void)
{
    return s_probe_next <= 254 || s_probe_id;
}

void clu_mysensors_refresh(void)
{
    if (!s_mx) {
        return;
    }
    int64_t now = now_ms();
    int known = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int id = 1; id < MS_BROADCAST; id++) {
        known += id_taken((uint8_t)id);
    }
    probe_start(now, 0); // a node heard before now may be gone already (e.g. back under a new id)
    xSemaphoreGive(s_mx);
    // the UART task asks the nodes; normally done in known x (answer time) + the silent ones x 0.5 s
    const int64_t deadline = now + (int64_t)known * (PROBE_WAIT_MS + 100) + 1000;
    for (;;) {
        xSemaphoreTake(s_mx, portMAX_DELAY);
        bool running = probe_running();
        xSemaphoreGive(s_mx);
        if (!running) {
            break;
        }
        if (now_ms() >= deadline) {
            ESP_LOGW(TAG, "discovery: node check not finished in time, no module removed");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(PROBE_POLL_MS));
    }
    uint8_t alive[32] = {0};
    int heard = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < MS_MAX_NODES; i++) {
        const ms_node_t *n = &s_nodes[i];
        if (n->used && n->last_ms >= s_probe_since) {
            alive[n->id / 8] |= (uint8_t)(1u << (n->id % 8));
            heard++;
        }
    }
    ids_forget(alive);
    xSemaphoreGive(s_mx);
    ESP_LOGI(TAG, "discovery: %d of %d node(s) answered the node check", heard, known);
    ms_vmod_prune(alive); // no s_mx held: ms_vmod takes its own lock, the gateway task the other way round
}

/** @brief Periodic work: link check, script messages to the send queue, send when the bus is quiet. */
static void tick(int64_t now)
{
    if (now >= s_check_ms) { // judged only when something came in: nodes are quiet most of the time
        if (s_win.bytes) {
            check_link();
        }
        memset(&s_win, 0, sizeof s_win);
        s_check_ms = now + GW_CHECK_MS;
    }
    ms_msg_t m;
    while (s_gwq_n < GW_QUEUE && xQueueReceive(s_txq, &m, 0) == pdTRUE) {
        wire(&m);
    }
    probe(now);
    gw_flush(now);
}

static void ms_task(void *arg)
{
    uint8_t buf[64];
    for (;;) {
        int n = uart_read_bytes(s_cfg.uart_num, buf, sizeof buf, pdMS_TO_TICKS(20));
        int64_t now = now_ms();
        if (n > 0) {
            gw_bytes(buf, (size_t)n, now);
        }
        tick(now);
    }
}

bool clu_mysensors_send(const ms_msg_t *m)
{
    if (!s_txq || strlen(m->payload) > MS_MAX_PAYLOAD || strpbrk(m->payload, "\r\n")) {
        return false;
    }
    if (m->cmd == MS_C_SET) {
        // a node asking for this value later (C_REQ, e.g. after a reboot) gets it
        xSemaphoreTake(s_mx, portMAX_DELAY);
        value_set(m->node, m->child, m->type, m->payload, now_ms());
        xSemaphoreGive(s_mx);
        ms_vmod_value(m->node, m->child, m->type, m->payload); // an emulated output follows it
    }
    return xQueueSend(s_txq, m, 0) == pdTRUE;
}

/** @brief ms_vmod_cfg_t::send. */
static bool vmod_send(uint8_t node, uint8_t child, uint8_t cmd, uint8_t vtype, const char *payload)
{
    ms_msg_t m = {.node = node, .child = child, .cmd = cmd, .type = vtype};
    strlcpy(m.payload, payload, sizeof m.payload);
    return clu_mysensors_send(&m);
}

bool clu_mysensors_connected(void)
{
    return s_txq != NULL; // the CLU is the gateway: the network is there as long as it runs
}

// ---------------------------------------------------------------------------------------------
// Lua (__clu.ms_*), used by mysensors.lua
// ---------------------------------------------------------------------------------------------
static uint8_t arg_u8(lua_State *L, int i)
{
    lua_Integer v = luaL_checkinteger(L, i);
    luaL_argcheck(L, v >= 0 && v <= 255, i, "0..255 expected");
    return (uint8_t)v;
}

/** @brief `__clu.ms_recv()` -> node, child, cmd, ack, type, payload | nil */
static int l_recv(lua_State *L)
{
    ms_msg_t m;
    if (xQueueReceive(s_luaq, &m, 0) != pdTRUE) {
        return 0;
    }
    lua_pushinteger(L, m.node);
    lua_pushinteger(L, m.child);
    lua_pushinteger(L, m.cmd);
    lua_pushinteger(L, m.ack);
    lua_pushinteger(L, m.type);
    lua_pushstring(L, m.payload);
    return 6;
}

/** @brief `__clu.ms_send(node, child, cmd, type, payload [, ack])` -> queued */
static int l_send(lua_State *L)
{
    ms_msg_t m = {
        .node = arg_u8(L, 1), .child = arg_u8(L, 2), .cmd = arg_u8(L, 3), .type = arg_u8(L, 4),
        .ack = lua_toboolean(L, 6) ? 1 : 0,
    };
    size_t len;
    const char *p = luaL_optlstring(L, 5, "", &len);
    luaL_argcheck(L, m.cmd <= MS_C_STREAM, 3, "command 0..4 expected");
    luaL_argcheck(L, len <= MS_MAX_PAYLOAD, 5, "payload longer than 25 bytes");
    luaL_argcheck(L, strlen(p) == len && !strpbrk(p, "\r\n"), 5, "payload with a line break / NUL");
    memcpy(m.payload, p, len + 1);
    lua_pushboolean(L, clu_mysensors_send(&m));
    return 1;
}

/** @brief `__clu.ms_get(node, child, type)` -> payload, age_ms | nil */
static int l_get(lua_State *L)
{
    uint8_t node = arg_u8(L, 1), child = arg_u8(L, 2), type = arg_u8(L, 3);
    char p[MS_MAX_PAYLOAD + 1];
    int64_t ms = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    const ms_value_t *v = value_find(node, child, type);
    if (v) {
        strlcpy(p, v->payload, sizeof p);
        ms = v->ms;
    }
    xSemaphoreGive(s_mx);
    if (!v) {
        return 0;
    }
    lua_pushstring(L, p);
    lua_pushinteger(L, (lua_Integer)(now_ms() - ms));
    return 2;
}

/** @brief `__clu.ms_nodes()` -> { [id] = { sketch, version, lib, type, battery, age_ms,
 *  children = { [child] = { type, desc } } } }. Node 0 is the gateway. */
static int l_nodes(lua_State *L)
{
    // Snapshot first: a Lua error (out of memory) must not leave the lock held.
    ms_node_t *nodes = malloc(sizeof s_nodes);
    ms_child_t *children = malloc(sizeof s_children);
    if (!nodes || !children) {
        free(nodes);
        free(children);
        return luaL_error(L, "out of memory");
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    memcpy(nodes, s_nodes, sizeof s_nodes);
    memcpy(children, s_children, sizeof s_children);
    xSemaphoreGive(s_mx);
    int64_t now = now_ms();

    lua_newtable(L);
    for (int i = 0; i < MS_MAX_NODES; i++) {
        const ms_node_t *n = &nodes[i];
        if (!n->used) {
            continue;
        }
        lua_newtable(L);
        lua_pushstring(L, n->sketch);
        lua_setfield(L, -2, "sketch");
        lua_pushstring(L, n->version);
        lua_setfield(L, -2, "version");
        lua_pushstring(L, n->lib);
        lua_setfield(L, -2, "lib");
        lua_pushinteger(L, n->type);
        lua_setfield(L, -2, "type");
        if (n->battery >= 0) {
            lua_pushinteger(L, n->battery);
            lua_setfield(L, -2, "battery");
        }
        lua_pushinteger(L, (lua_Integer)(now - n->last_ms));
        lua_setfield(L, -2, "age_ms");
        lua_newtable(L);
        lua_setfield(L, -2, "children");
        lua_rawseti(L, -2, n->id);
    }
    for (int i = 0; i < MS_MAX_CHILDREN; i++) {
        const ms_child_t *c = &children[i];
        if (!c->used) {
            continue;
        }
        if (lua_rawgeti(L, -1, c->node) == LUA_TTABLE) { // node may have been dropped (registry full)
            lua_getfield(L, -1, "children");
            lua_newtable(L);
            lua_pushinteger(L, c->type);
            lua_setfield(L, -2, "type");
            lua_pushstring(L, c->desc);
            lua_setfield(L, -2, "desc");
            lua_rawseti(L, -2, c->child);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    free(nodes);
    free(children);
    return 1;
}

/** @brief `__clu.ms_trace(on)`: log every line in both directions at INFO. */
static int l_trace(lua_State *L)
{
    s_trace = lua_toboolean(L, 1);
    ESP_LOGI(TAG, "trace %s", s_trace ? "on" : "off");
    return 0;
}

/** @brief `__clu.ms_status()` -> { connected, version, rx, tx, dropped, bad, rx_bytes, link, trace } */
static int l_status(lua_State *L)
{
    char version[sizeof s_gw_version];
    xSemaphoreTake(s_mx, portMAX_DELAY);
    strlcpy(version, s_gw_version, sizeof version);
    lua_Integer rx = s_rx, tx = s_tx, dropped = s_dropped, bad = s_bad, bytes = s_rx_bytes;
    link_t link = s_link;
    xSemaphoreGive(s_mx);
    lua_newtable(L);
    lua_pushboolean(L, clu_mysensors_connected());
    lua_setfield(L, -2, "connected");
    lua_pushstring(L, version);
    lua_setfield(L, -2, "version");
    lua_pushinteger(L, rx);
    lua_setfield(L, -2, "rx");
    lua_pushinteger(L, tx);
    lua_setfield(L, -2, "tx");
    lua_pushinteger(L, dropped);
    lua_setfield(L, -2, "dropped");
    lua_pushinteger(L, bad);
    lua_setfield(L, -2, "bad");
    lua_pushinteger(L, bytes);
    lua_setfield(L, -2, "rx_bytes");
    lua_pushstring(L, LINK_NAME[link]);
    lua_setfield(L, -2, "link");
    lua_pushboolean(L, s_trace);
    lua_setfield(L, -2, "trace");
    return 1;
}

/** @brief `__clu.ms_slots()` -> { { module, sn, kind = "din" | "dout", channel, node, child, vtype } }
 *  (module and channel 1-based, as OM numbers them) */
static int l_slots(lua_State *L)
{
    ms_vmod_slot_t *v = malloc(sizeof *v * MS_VMOD_MAX * MS_VMOD_SLOTS);
    if (!v) {
        return luaL_error(L, "out of memory");
    }
    int n = ms_vmod_list(v, MS_VMOD_MAX * MS_VMOD_SLOTS);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_createtable(L, 0, 7);
        lua_pushinteger(L, v[i].module + 1);
        lua_setfield(L, -2, "module");
        lua_pushinteger(L, v[i].sn);
        lua_setfield(L, -2, "sn");
        lua_pushstring(L, v[i].kind == MS_SLOT_DIN ? "din" : "dout");
        lua_setfield(L, -2, "kind");
        lua_pushinteger(L, v[i].channel + 1);
        lua_setfield(L, -2, "channel");
        lua_pushinteger(L, v[i].node);
        lua_setfield(L, -2, "node");
        lua_pushinteger(L, v[i].child);
        lua_setfield(L, -2, "child");
        lua_pushinteger(L, v[i].vtype);
        lua_setfield(L, -2, "vtype");
        lua_rawseti(L, -2, i + 1);
    }
    free(v);
    return 1;
}

/** @brief `__clu.ms_unmap(node [, child])` -> slots freed */
static int l_unmap(lua_State *L)
{
    uint8_t node = arg_u8(L, 1);
    uint8_t child = lua_isnoneornil(L, 2) ? MS_BROADCAST : arg_u8(L, 2);
    lua_pushinteger(L, ms_vmod_unmap(node, child));
    return 1;
}

static const luaL_Reg s_fns[] = {
    {"ms_trace", l_trace},
    {"ms_slots", l_slots},
    {"ms_unmap", l_unmap},
    {"ms_recv", l_recv},
    {"ms_send", l_send},
    {"ms_get", l_get},
    {"ms_nodes", l_nodes},
    {"ms_status", l_status},
    {NULL, NULL},
};

esp_err_t clu_mysensors_init(const clu_mysensors_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_trace = cfg->trace;
    ms_rs485_rx_init(&s_frx, MS_GATEWAY);
    s_gwq_head = s_gwq_n = 0;
    s_check_ms = 0;
    strlcpy(s_gw_version, "CLU RS485 gateway", sizeof s_gw_version);
    s_mx = xSemaphoreCreateMutex();
    s_luaq = xQueueCreate(MS_LUA_QUEUE, sizeof(ms_msg_t));
    s_txq = xQueueCreate(MS_TX_QUEUE, sizeof(ms_msg_t));
    if (!s_mx || !s_luaq || !s_txq) {
        return ESP_ERR_NO_MEM;
    }
    ids_load();
    probe_start(now_ms(), PROBE_DELAY_MS);
    const ms_vmod_cfg_t vc = {.max = cfg->max_modules, .send = vmod_send};
    ms_vmod_init(&vc); // before the bus master starts: its endpoint must see the first DISCOVER
    clu_lua_register_ext(s_fns, mysensors_lua_start, mysensors_lua_end - mysensors_lua_start - 1, "=mysensors");

    const uart_config_t uc = {
        .baud_rate = cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(cfg->uart_num, 1024, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(cfg->uart_num, &uc);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, cfg->de_pin, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        err = uart_set_mode(cfg->uart_num, cfg->de_pin >= 0 ? UART_MODE_RS485_HALF_DUPLEX : UART_MODE_UART);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d setup failed: %s", cfg->uart_num, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "RS485 network gateway (node 0) on UART%d tx=%d rx=%d de=%d @%d", cfg->uart_num, cfg->tx_pin,
             cfg->rx_pin, cfg->de_pin, cfg->baud);
    if (xTaskCreate(ms_task, "mysensors", 4096, NULL, 5, NULL) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
