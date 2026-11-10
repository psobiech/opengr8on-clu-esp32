/**
 * @file clu_mqtt.c
 * @brief MQTT client (esp-mqtt) and the C side of mqtt.lua.
 *
 * The MQTT task only queues inbound commands; mqtt.lua (Lua task) polls them with
 * `__clu.mqtt_recv()`, publishes state and Home Assistant discovery, and republishes everything when
 * the connection generation changes. Retained `<root>/status` online/offline is the availability
 * topic (LWT).
 */
#include "clu_mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt";

extern const char mqtt_lua_start[] asm("_binary_mqtt_lua_start");
extern const char mqtt_lua_end[] asm("_binary_mqtt_lua_end");

// Bool Kconfig symbols are undefined when off: usable in expressions through these.
#if CONFIG_CLU_MQTT_HA_DISCOVERY
#define HA_DISCOVERY 1
#define HA_PREFIX    CONFIG_CLU_MQTT_HA_PREFIX
#else
#define HA_DISCOVERY 0
#define HA_PREFIX    ""
#endif
#if CONFIG_CLU_MQTT_OBJECT_API
#define OBJECT_API 1
#else
#define OBJECT_API 0
#endif

#define MAX_TOPIC   128
#define MAX_PAYLOAD 256 // inbound only: commands are short ("ON", "42", "STOP")
#define RX_QUEUE    16
#define CA_MAX      (16 * 1024)

/** @brief Inbound message queued for Lua; topic and payload point into buf. */
typedef struct {
    char *topic;
    char *payload;
    char buf[]; // topic \0 payload \0
} rx_msg_t;

static clu_mqtt_cfg_t s_cfg;
static esp_mqtt_client_handle_t s_client;
static QueueHandle_t s_rx;
static char s_root[64];    // <base>/CLU<sn>
static char s_status[80];  // <root>/status (LWT / birth)
static char s_client_id[48];
static char *s_ca_pem;
static bool s_enabled = true; // UseCloud interface default
static bool s_started;
static bool s_running;
static volatile bool s_connected;
static volatile int s_gen; // bumped on every (re)connect: the Lua side republishes everything

bool clu_mqtt_connected(void)
{
    return s_connected;
}

// ---------------------------------------------------------------------------
// esp-mqtt events (MQTT task)
// ---------------------------------------------------------------------------
/**
 * @brief Subscribe to the command topics (`<root>/+/set`, position, system tree, object API, HA birth).
 */
static void subscribe_all(esp_mqtt_client_handle_t c)
{
    char t[MAX_TOPIC];
    snprintf(t, sizeof t, "%s/+/set", s_root);
    esp_mqtt_client_subscribe(c, t, 0);
    snprintf(t, sizeof t, "%s/+/position/set", s_root);
    esp_mqtt_client_subscribe(c, t, 0);
    // Firmware entities live in a tree: "<root>/system/clu/restart/set".
    snprintf(t, sizeof t, "%s/system/+/+/set", s_root);
    esp_mqtt_client_subscribe(c, t, 0);
#if CONFIG_CLU_MQTT_OBJECT_API
    // "execute/+" is one level: our own ".../execute/<idx>/result" replies never loop back.
    snprintf(t, sizeof t, "%s/+/feature/+/get", s_root);
    esp_mqtt_client_subscribe(c, t, 0);
    snprintf(t, sizeof t, "%s/+/feature/+/set", s_root);
    esp_mqtt_client_subscribe(c, t, 0);
    snprintf(t, sizeof t, "%s/+/execute/+", s_root);
    esp_mqtt_client_subscribe(c, t, 0);
#endif
#if CONFIG_CLU_MQTT_HA_DISCOVERY
    // HA birth message: "online" after a Home Assistant restart -> discovery is resent.
    snprintf(t, sizeof t, "%s/status", CONFIG_CLU_MQTT_HA_PREFIX);
    esp_mqtt_client_subscribe(c, t, 0);
#endif
}

/** @brief Queue an inbound message for Lua; fragmented or oversized messages are dropped. */
static void on_data(const esp_mqtt_event_t *e)
{
    // Fragmented (larger than the RX buffer) or oversized messages are not commands: drop.
    if (e->current_data_offset != 0 || e->data_len != e->total_data_len || e->topic_len <= 0 ||
        e->topic_len >= MAX_TOPIC || e->data_len >= MAX_PAYLOAD) {
        ESP_LOGW(TAG, "dropped inbound message (%d/%d bytes)", e->topic_len, e->total_data_len);
        return;
    }
    rx_msg_t *m = malloc(sizeof *m + e->topic_len + 1 + e->data_len + 1);
    if (!m) {
        return;
    }
    m->topic = m->buf;
    memcpy(m->topic, e->topic, e->topic_len);
    m->topic[e->topic_len] = 0;
    m->payload = m->topic + e->topic_len + 1;
    memcpy(m->payload, e->data, e->data_len);
    m->payload[e->data_len] = 0;
    if (xQueueSend(s_rx, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "inbound queue full, dropped %s", m->topic);
        free(m);
    }
}

/** @brief esp-mqtt event handler: birth message, subscriptions and connection state. */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to %s", CONFIG_CLU_MQTT_BROKER_URI);
        esp_mqtt_client_publish(e->client, s_status, "online", 0, 1, 1);
        subscribe_all(e->client);
        s_gen++;
        s_connected = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) {
            ESP_LOGW(TAG, "disconnected");
        }
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        on_data(e);
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "broker refused connection (code %d)", e->error_handle->connect_return_code);
        } else if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "transport error: %s (tls 0x%x)", esp_err_to_name(e->error_handle->esp_tls_last_esp_err),
                     e->error_handle->esp_tls_stack_err);
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Connection control (Lua task)
// ---------------------------------------------------------------------------
/**
 * @brief Read a whole file (CA PEM) into a NUL-terminated heap buffer.
 * @return Buffer, or NULL if missing, empty or at least @p max bytes.
 */
static char *read_file(const char *path, size_t max)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) {
        return NULL;
    }
    char *buf = malloc(max + 1);
    size_t n = buf ? fread(buf, 1, max, f) : 0;
    fclose(f);
    if (!buf || n == 0 || n == max) { // empty or too big to be a CA
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    return buf;
}

/**
 * @brief Create the esp-mqtt client from menuconfig (credentials, LWT, TLS CA file or certificate bundle).
 */
static bool client_create(void)
{
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = CONFIG_CLU_MQTT_BROKER_URI,
        .credentials.client_id = s_client_id,
        .session.keepalive = CONFIG_CLU_MQTT_KEEPALIVE,
        .session.last_will = {.topic = s_status, .msg = "offline", .qos = 1, .retain = 1},
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = 2048,
    };
    if (CONFIG_CLU_MQTT_USERNAME[0]) {
        mc.credentials.username = CONFIG_CLU_MQTT_USERNAME;
        mc.credentials.authentication.password = CONFIG_CLU_MQTT_PASSWORD;
    }
    if (strncmp(CONFIG_CLU_MQTT_BROKER_URI, "mqtts://", 8) == 0 ||
        strncmp(CONFIG_CLU_MQTT_BROKER_URI, "wss://", 6) == 0) {
        s_ca_pem = read_file(s_cfg.ca_path, CA_MAX);
        if (s_ca_pem) {
            ESP_LOGI(TAG, "TLS: CA from %s", s_cfg.ca_path);
            mc.broker.verification.certificate = s_ca_pem;
        } else {
            mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed (URI %s)", CONFIG_CLU_MQTT_BROKER_URI);
        return false;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    return true;
}

/** @brief Start or stop the connection to match started && UseCloud && broker configured. */
static void apply(void)
{
    bool want = s_started && s_enabled && CONFIG_CLU_MQTT_BROKER_URI[0];
    if (want == s_running) {
        return;
    }
    if (want) {
        if (!s_client && !client_create()) {
            return;
        }
        ESP_LOGI(TAG, "UseCloud on: connecting to %s as %s", CONFIG_CLU_MQTT_BROKER_URI, s_client_id);
        s_running = esp_mqtt_client_start(s_client) == ESP_OK;
    } else {
        ESP_LOGI(TAG, "UseCloud off: disconnecting");
        if (s_connected) {
            // Clean disconnect skips the LWT: mark entities unavailable ourselves.
            esp_mqtt_client_publish(s_client, s_status, "offline", 0, 1, 1);
        }
        esp_mqtt_client_stop(s_client);
        s_running = false;
        s_connected = false;
    }
}

void clu_mqtt_set_enabled(bool on)
{
    s_enabled = on;
    apply();
}

void clu_mqtt_start(void)
{
    s_started = true;
    if (!CONFIG_CLU_MQTT_BROKER_URI[0]) {
        ESP_LOGI(TAG, "no broker configured (menuconfig CLU_MQTT_BROKER_URI), MQTT off");
    }
    apply();
}

// ---------------------------------------------------------------------------
// Lua bindings (merged into __clu, used by mqtt.lua only)
// ---------------------------------------------------------------------------

// __clu.mqtt_cfg() -> table
static int l_cfg(lua_State *L)
{
    lua_newtable(L);
    lua_pushstring(L, s_root);
    lua_setfield(L, -2, "root");
    lua_pushstring(L, s_status);
    lua_setfield(L, -2, "status");
    lua_pushboolean(L, HA_DISCOVERY);
    lua_setfield(L, -2, "ha");
    lua_pushstring(L, HA_PREFIX);
    lua_setfield(L, -2, "prefix");
    lua_pushinteger(L, CONFIG_CLU_MQTT_SCAN_MS);
    lua_setfield(L, -2, "scan_ms");
    lua_pushinteger(L, s_cfg.sn);
    lua_setfield(L, -2, "sn");
#if CONFIG_CLU_MQTT_DIAGNOSTICS
    lua_pushinteger(L, CONFIG_CLU_MQTT_DIAG_INTERVAL * 1000);
    lua_setfield(L, -2, "diag_ms"); // absent = no internals entities
#endif
    lua_pushboolean(L, OBJECT_API);
    lua_setfield(L, -2, "api");
    lua_pushstring(L, s_cfg.fw_version ? s_cfg.fw_version : "");
    lua_setfield(L, -2, "fw");
    return 1;
}

// __clu.mqtt_state() -> connected, generation
static int l_state(lua_State *L)
{
    lua_pushboolean(L, s_connected);
    lua_pushinteger(L, s_gen);
    return 2;
}

// __clu.mqtt_publish(topic, payload, retain, qos) -> queued
static int l_publish(lua_State *L)
{
    const char *topic = luaL_checkstring(L, 1);
    size_t len;
    const char *payload = luaL_checklstring(L, 2, &len);
    int retain = lua_toboolean(L, 3);
    int qos = (int)luaL_optinteger(L, 4, 0);
    if (!s_connected || qos < 0 || qos > 1) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // Non-blocking: goes to the outbox, the MQTT task sends it.
    int id = esp_mqtt_client_enqueue(s_client, topic, payload, (int)len, qos, retain, true);
    lua_pushboolean(L, id >= 0);
    return 1;
}

// __clu.mqtt_outbox() -> bytes waiting in the outbox
static int l_outbox(lua_State *L)
{
    lua_pushinteger(L, s_client ? esp_mqtt_client_get_outbox_size(s_client) : 0);
    return 1;
}

// __clu.mqtt_recv() -> topic, payload | nil
static int l_recv(lua_State *L)
{
    rx_msg_t *m;
    if (!s_rx || xQueueReceive(s_rx, &m, 0) != pdTRUE) {
        return 0;
    }
    lua_pushstring(L, m->topic);
    lua_pushstring(L, m->payload);
    free(m);
    return 2;
}

static const luaL_Reg s_fns[] = {
    {"mqtt_cfg", l_cfg},         {"mqtt_state", l_state}, {"mqtt_publish", l_publish},
    {"mqtt_outbox", l_outbox},   {"mqtt_recv", l_recv},   {NULL, NULL},
};

void clu_mqtt_init(const clu_mqtt_cfg_t *cfg)
{
    s_cfg = *cfg;
    snprintf(s_root, sizeof s_root, "%s/CLU%u", CONFIG_CLU_MQTT_TOPIC_BASE, s_cfg.sn);
    snprintf(s_status, sizeof s_status, "%s/status", s_root);
    if (CONFIG_CLU_MQTT_CLIENT_ID[0]) {
        strlcpy(s_client_id, CONFIG_CLU_MQTT_CLIENT_ID, sizeof s_client_id);
    } else {
        snprintf(s_client_id, sizeof s_client_id, "opengr8on-CLU%u", s_cfg.sn);
    }
    s_rx = xQueueCreate(RX_QUEUE, sizeof(rx_msg_t *));
    clu_lua_register_ext(s_fns, mqtt_lua_start, mqtt_lua_end - mqtt_lua_start - 1, "=mqtt");
}
