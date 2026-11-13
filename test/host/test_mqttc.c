// Host tests for the MQTT link (clu_mqtt.c, included: event handler and Lua bindings are static)
// against a fake esp-mqtt client. The Kconfig broker settings are bound to variables so one
// binary covers plain / TLS / authenticated brokers; HA discovery, the object API and
// diagnostics are compiled in.
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *cfg_uri = "", *cfg_user = "", *cfg_pass = "", *cfg_client_id = "";
#define CONFIG_CLU_MQTT_BROKER_URI    cfg_uri
#define CONFIG_CLU_MQTT_USERNAME      cfg_user
#define CONFIG_CLU_MQTT_PASSWORD      cfg_pass
#define CONFIG_CLU_MQTT_CLIENT_ID     cfg_client_id
#define CONFIG_CLU_MQTT_TOPIC_BASE    "grenton"
#define CONFIG_CLU_MQTT_KEEPALIVE     30
#define CONFIG_CLU_MQTT_SCAN_MS       200
#define CONFIG_CLU_MQTT_HA_DISCOVERY  1
#define CONFIG_CLU_MQTT_HA_PREFIX     "homeassistant"
#define CONFIG_CLU_MQTT_OBJECT_API    1
#define CONFIG_CLU_MQTT_DIAGNOSTICS   1
#define CONFIG_CLU_MQTT_DIAG_INTERVAL 60

#include "../../components/clu_mqtt/clu_mqtt.c"

#include "lualib.h"

// The firmware embeds mqtt.lua via EMBED_TXTFILES (_binary_mqtt_lua_start/_end, NUL-terminated).
__asm__(".pushsection .rodata\n"
        ".global _binary_mqtt_lua_start\n_binary_mqtt_lua_start:\n.asciz \"return 1\"\n"
        ".global _binary_mqtt_lua_end\n_binary_mqtt_lua_end:\n"
        ".popsection\n");

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- FreeRTOS queue
struct host_queue {
    size_t item, cap, head, n;
    uint8_t *buf;
};
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item)
{
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

// ---------------------------------------------------------------- clu_lua
static const luaL_Reg *s_ext;
static const char *s_ext_src;
static size_t s_ext_len;
static char s_ext_name[16];
void clu_lua_register_ext(const luaL_Reg *fns, const char *src, size_t len, const char *name)
{
    s_ext = fns;
    s_ext_src = src;
    s_ext_len = len;
    snprintf(s_ext_name, sizeof s_ext_name, "%s", name);
}

// ---------------------------------------------------------------- fake esp-mqtt
struct esp_mqtt_client { int dummy; };
static struct esp_mqtt_client s_fake;
static esp_mqtt_client_config_t s_mc;
static esp_event_handler_t s_handler;
static int s_inits, s_starts, s_stops, s_init_fail, s_start_fail, s_enqueue_fail, s_outbox = 77;
static char s_subs[16][128], s_pubs[8][160];
static int s_nsubs, s_npubs;
static struct {
    char topic[128], payload[64];
    int len, qos, retain;
    bool store;
} s_enq;

esp_err_t esp_crt_bundle_attach(void *conf) { return ESP_OK; }
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg)
{
    s_inits++;
    s_mc = *cfg;
    return s_init_fail ? NULL : &s_fake;
}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, int32_t id, esp_event_handler_t fn, void *arg)
{
    s_handler = fn;
    return ESP_OK;
}
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c)
{
    s_starts++;
    return s_start_fail ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c)
{
    s_stops++;
    return ESP_OK;
}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char *topic, int qos)
{
    snprintf(s_subs[s_nsubs++ % 16], sizeof s_subs[0], "%s", topic);
    return 1;
}
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic, const char *data, int len, int qos, int retain)
{
    snprintf(s_pubs[s_npubs++ % 8], sizeof s_pubs[0], "%s=%s q%d r%d", topic, data, qos, retain);
    return 1;
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t c, const char *topic, const char *data, int len, int qos,
                            int retain, bool store)
{
    snprintf(s_enq.topic, sizeof s_enq.topic, "%s", topic);
    snprintf(s_enq.payload, sizeof s_enq.payload, "%.*s", len, data);
    s_enq.len = len;
    s_enq.qos = qos;
    s_enq.retain = retain;
    s_enq.store = store;
    return s_enqueue_fail ? -1 : 5;
}
int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t c) { return s_outbox; }

static void event(esp_mqtt_event_id_t id, esp_mqtt_event_t *e)
{
    esp_mqtt_event_t empty = {.client = &s_fake};
    s_handler(NULL, "MQTT_EVENTS", id, e ? e : &empty);
}

static void data_event(const char *topic, const char *payload, int offset, int total)
{
    esp_mqtt_event_t e = {.client = &s_fake, .topic = (char *)topic, .topic_len = (int)strlen(topic),
                          .data = (char *)payload, .data_len = (int)strlen(payload),
                          .total_data_len = total >= 0 ? total : (int)strlen(payload), .current_data_offset = offset};
    event(MQTT_EVENT_DATA, &e);
}

// ---------------------------------------------------------------- Lua helpers
static lua_State *L;

static void lua_call_fn(const char *fn, int nargs, int nres)
{
    lua_getglobal(L, fn);
    lua_insert(L, -1 - nargs);
    lua_call(L, nargs, nres);
}

static bool publish(const char *topic, const char *payload, int retain, int qos)
{
    lua_pushstring(L, topic);
    lua_pushstring(L, payload);
    lua_pushboolean(L, retain);
    lua_pushinteger(L, qos);
    lua_call_fn("mqtt_publish", 4, 1);
    bool r = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return r;
}

static void reconfigure(const char *uri)
{
    cfg_uri = uri;
    s_client = NULL;
    s_running = s_connected = false;
    s_started = false;
    free(s_ca_pem);
    s_ca_pem = NULL;
}

// ---------------------------------------------------------------- tests
int main(void)
{
    // Lua bindings before init: no queue / client yet
    clu_mqtt_cfg_t cfg = {.sn = 330017203, .fw_version = "1.2.3", .ca_path = NULL};
    L = luaL_newstate();
    luaL_openlibs(L);
    check("recv / outbox before init -> nothing", l_recv(L) == 0 && (l_outbox(L), lua_tointeger(L, -1) == 0));
    lua_settop(L, 0);

    clu_mqtt_init(&cfg);
    check("init: topic root, status topic, default client id",
          !strcmp(s_root, "grenton/CLU330017203") && !strcmp(s_status, "grenton/CLU330017203/status") &&
              !strcmp(s_client_id, "opengr8on-CLU330017203"));
    check("init: Lua bindings + embedded mqtt.lua registered",
          s_ext && !strcmp(s_ext_name, "=mqtt") && s_ext_len == 8 && !strncmp(s_ext_src, "return 1", 8));
    for (const luaL_Reg *r = s_ext; r->name; r++) {
        lua_register(L, r->name, r->func);
    }
    cfg_client_id = "my-clu";
    clu_mqtt_init(&cfg);
    check("init: configured client id", !strcmp(s_client_id, "my-clu"));

    // ---- no broker: nothing started
    clu_mqtt_start();
    check("no broker configured: no client", s_inits == 0 && !s_running);

    // ---- plain broker, no credentials
    reconfigure("mqtt://broker.lan");
    clu_mqtt_start();
    check("start: client with URI, client id, keepalive, retained LWT 'offline'",
          s_inits == 1 && s_starts == 1 && s_running && !strcmp(s_mc.broker.address.uri, "mqtt://broker.lan") &&
              !strcmp(s_mc.credentials.client_id, "my-clu") && s_mc.session.keepalive == 30 &&
              !strcmp(s_mc.session.last_will.topic, "grenton/CLU330017203/status") &&
              !strcmp(s_mc.session.last_will.msg, "offline") && s_mc.session.last_will.retain &&
              !s_mc.credentials.username && !s_mc.broker.verification.certificate &&
              !s_mc.broker.verification.crt_bundle_attach);
    clu_mqtt_start();
    check("start again: no second client", s_inits == 1 && s_starts == 1);

    // ---- events
    event(MQTT_EVENT_CONNECTED, NULL);
    check("connected: 'online' retained, generation bumped", clu_mqtt_connected() && s_gen == 1 &&
                                                             !strcmp(s_pubs[0], "grenton/CLU330017203/status=online q1 r1"));
    static const char *want_subs[] = {
        "grenton/CLU330017203/+/set", "grenton/CLU330017203/+/position/set", "grenton/CLU330017203/system/+/+/set",
        "grenton/CLU330017203/+/feature/+/get", "grenton/CLU330017203/+/feature/+/set",
        "grenton/CLU330017203/+/execute/+", "homeassistant/status",
    };
    bool subs = s_nsubs == 7;
    for (int i = 0; subs && i < 7; i++) {
        subs = !strcmp(s_subs[i], want_subs[i]);
    }
    check("connected: command, object API and HA birth topics subscribed", subs);

    data_event("grenton/CLU330017203/DOU1/set", "ON", 0, -1);
    data_event("grenton/CLU330017203/big/set", "x", 0, 5000);   // fragmented
    data_event("grenton/CLU330017203/frag/set", "x", 100, 101); // later fragment
    char big[300];
    memset(big, 'p', sizeof big - 1);
    big[sizeof big - 1] = 0;
    data_event("grenton/CLU330017203/long/set", big, 0, -1);    // payload too long
    data_event("", "x", 0, -1);                                // no topic
    lua_call_fn("mqtt_recv", 0, 2);
    check("inbound command delivered to Lua (topic, payload)",
          !strcmp(lua_tostring(L, -2), "grenton/CLU330017203/DOU1/set") && !strcmp(lua_tostring(L, -1), "ON"));
    lua_settop(L, 0);
    lua_call_fn("mqtt_recv", 0, LUA_MULTRET);
    check("fragmented / oversized / topic-less messages dropped", lua_gettop(L) == 0);

    for (int i = 0; i < RX_QUEUE + 2; i++) {
        data_event("grenton/CLU330017203/X/set", "1", 0, -1);
    }
    int got = 0;
    for (;;) {
        lua_settop(L, 0);
        lua_call_fn("mqtt_recv", 0, LUA_MULTRET);
        if (!lua_gettop(L)) {
            break;
        }
        got++;
    }
    check("inbound queue full: excess dropped", got == RX_QUEUE);

    esp_mqtt_error_codes_t refused = {.error_type = MQTT_ERROR_TYPE_CONNECTION_REFUSED, .connect_return_code = 5};
    esp_mqtt_error_codes_t tls = {.error_type = MQTT_ERROR_TYPE_TCP_TRANSPORT, .esp_tls_last_esp_err = ESP_FAIL};
    esp_mqtt_event_t e1 = {.client = &s_fake, .error_handle = &refused}, e2 = {.client = &s_fake, .error_handle = &tls};
    event(MQTT_EVENT_ERROR, &e1);
    event(MQTT_EVENT_ERROR, &e2);
    event(MQTT_EVENT_ERROR, NULL);
    event(MQTT_EVENT_PUBLISHED, NULL);
    check("error / other events: logged only, still connected", clu_mqtt_connected());

    // ---- Lua bindings
    lua_settop(L, 0);
    lua_call_fn("mqtt_cfg", 0, 1);
    lua_getfield(L, 1, "root");
    lua_getfield(L, 1, "status");
    lua_getfield(L, 1, "ha");
    lua_getfield(L, 1, "prefix");
    lua_getfield(L, 1, "scan_ms");
    lua_getfield(L, 1, "sn");
    lua_getfield(L, 1, "diag_ms");
    lua_getfield(L, 1, "api");
    lua_getfield(L, 1, "fw");
    check("mqtt_cfg: root, status, HA prefix, scan / diag interval, sn, object API, fw",
          !strcmp(lua_tostring(L, 2), "grenton/CLU330017203") && !strcmp(lua_tostring(L, 3), s_status) &&
              lua_toboolean(L, 4) && !strcmp(lua_tostring(L, 5), "homeassistant") && lua_tointeger(L, 6) == 200 &&
              lua_tointeger(L, 7) == 330017203 && lua_tointeger(L, 8) == 60000 && lua_toboolean(L, 9) &&
              !strcmp(lua_tostring(L, 10), "1.2.3"));
    lua_settop(L, 0);
    clu_mqtt_cfg_t nofw = cfg;
    nofw.fw_version = NULL;
    s_cfg = nofw;
    lua_call_fn("mqtt_cfg", 0, 1);
    lua_getfield(L, 1, "fw");
    check("mqtt_cfg: no firmware version -> \"\"", !strcmp(lua_tostring(L, -1), ""));
    s_cfg = cfg;
    lua_settop(L, 0);

    lua_call_fn("mqtt_state", 0, 2);
    check("mqtt_state: connected, generation", lua_toboolean(L, 1) && lua_tointeger(L, 2) == 1);
    lua_settop(L, 0);

    check("mqtt_publish: queued in the outbox (non-blocking, stored)",
          publish("t/x", "42", 1, 1) && !strcmp(s_enq.topic, "t/x") && !strcmp(s_enq.payload, "42") &&
              s_enq.qos == 1 && s_enq.retain && s_enq.store);
    check("mqtt_publish: QoS 2 refused", !publish("t/x", "1", 0, 2));
    s_enqueue_fail = 1;
    check("mqtt_publish: outbox full -> false", !publish("t/x", "1", 0, 0));
    s_enqueue_fail = 0;
    lua_call_fn("mqtt_outbox", 0, 1);
    check("mqtt_outbox: bytes waiting", lua_tointeger(L, -1) == 77);
    lua_settop(L, 0);

    event(MQTT_EVENT_DISCONNECTED, NULL);
    event(MQTT_EVENT_DISCONNECTED, NULL);
    check("disconnected: state off, publish refused", !clu_mqtt_connected() && !publish("t/x", "1", 0, 0));

    // ---- UseCloud off / on
    event(MQTT_EVENT_CONNECTED, NULL);
    s_npubs = 0;
    clu_mqtt_set_enabled(false);
    check("UseCloud off: 'offline' published (clean disconnect skips the LWT), client stopped",
          s_stops == 1 && !s_running && !clu_mqtt_connected() && !strcmp(s_pubs[0], "grenton/CLU330017203/status=offline q1 r1"));
    clu_mqtt_set_enabled(false);
    check("UseCloud off again: nothing", s_stops == 1);
    clu_mqtt_set_enabled(true);
    check("UseCloud on: same client restarted", s_inits == 1 && s_starts == 2 && s_running);
    s_npubs = 0;
    clu_mqtt_set_enabled(false); // not connected: no 'offline'
    check("UseCloud off while not connected: no 'offline' publish", s_npubs == 0 && s_stops == 2);
    clu_mqtt_set_enabled(true);

    // ---- start failure, init failure
    reconfigure("mqtt://broker.lan");
    s_start_fail = 1;
    clu_mqtt_start();
    check("client start failure: not running", !s_running);
    s_start_fail = 0;
    reconfigure("mqtt://broker.lan");
    s_init_fail = 1;
    clu_mqtt_start();
    s_init_fail = 0;
    check("client init failure: not running, no start", !s_running && !s_client);

    // ---- credentials and TLS
    reconfigure("mqtts://broker.example:8883");
    cfg_user = "user";
    cfg_pass = "secret";
    char ca[] = "/tmp/clu_ca_XXXXXX";
    int fd = mkstemp(ca);
    if (write(fd, "-----BEGIN CERTIFICATE-----\n", 28) != 28) {
        perror("write");
    }
    close(fd);
    s_cfg.ca_path = ca;
    clu_mqtt_start();
    check("mqtts with a CA file: certificate from the file, credentials set",
          s_mc.broker.verification.certificate && !strncmp(s_mc.broker.verification.certificate, "-----BEGIN", 10) &&
              !s_mc.broker.verification.crt_bundle_attach && !strcmp(s_mc.credentials.username, "user") &&
              !strcmp(s_mc.credentials.authentication.password, "secret"));
    reconfigure("wss://broker.example/mqtt");
    s_cfg.ca_path = "/nonexistent/ca.pem";
    clu_mqtt_start();
    check("wss without a CA file: built-in certificate bundle",
          !s_mc.broker.verification.certificate && s_mc.broker.verification.crt_bundle_attach == esp_crt_bundle_attach);
    fd = open(ca, O_WRONLY | O_TRUNC);
    close(fd);
    reconfigure("mqtts://broker.example");
    s_cfg.ca_path = ca;
    clu_mqtt_start();
    check("empty CA file: certificate bundle", !s_mc.broker.verification.certificate);
    unlink(ca);

    // ---- network up before clu_mqtt_init() (IP event first): remembered, connected by init
    reconfigure("mqtt://early.lan");
    s_lock = NULL;
    int inits = s_inits;
    clu_mqtt_start();
    check("start before init: nothing yet", s_inits == inits && !s_running);
    clu_mqtt_init(&cfg);
    check("init after an early start: connects", s_inits == inits + 1 && s_running);
    clu_mqtt_start(); // SYSTEM.Init later
    check("SYSTEM.Init after the IP event: no second client", s_inits == inits + 1 && host_mutex_held == 0);

    // ---- broker / credentials from NVS settings (clu_mqtt_cfg_t) win over menuconfig
    reconfigure("mqtt://kconfig.lan");
    cfg_user = "kuser";
    s_cfg.broker_uri = "mqtt://nvs.lan";
    s_cfg.username = "nvsuser";
    s_cfg.password = "nvspass";
    clu_mqtt_start();
    check("NVS broker URI and credentials used",
          s_running && !strcmp(s_mc.broker.address.uri, "mqtt://nvs.lan") &&
              !strcmp(s_mc.credentials.username, "nvsuser") &&
              !strcmp(s_mc.credentials.authentication.password, "nvspass"));
    reconfigure("mqtt://kconfig.lan");
    s_cfg.broker_uri = "";
    s_cfg.username = s_cfg.password = NULL;
    clu_mqtt_start();
    check("NVS broker URI empty: MQTT off even with a menuconfig broker", !s_running);
    s_cfg.broker_uri = NULL;

    lua_close(L);
    free(s_ca_pem);
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
