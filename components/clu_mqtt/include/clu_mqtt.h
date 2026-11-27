/**
 * @file clu_mqtt.h
 * @brief MQTT link + Home Assistant discovery (mqtt.lua).
 *
 * Broker settings come from menuconfig ("OpenGr8on MQTT / Home Assistant");
 * the local CLU feature UseCloud (18) switches the connection on/off and CloudConnection (19)
 * reports it.
 *
 * Threading: all functions are thread-safe (clu_mqtt_set_enabled() and clu_mqtt_start() come from
 * the Lua task, clu_mqtt_set_online() from the network event loop and never blocks).
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief MQTT configuration. */
typedef struct {
    unsigned sn;          /**< CLU serial: topic root "<base>/CLU<sn>", default client id. */
    const char *fw_version;  /**< Firmware version reported to Home Assistant. */
    const char *model;       /**< Board name: Home Assistant device model (NULL = "OpenGr8on CLU"). */
    const char *hw_version;  /**< Chip and revision: Home Assistant device hw_version (NULL = none). */
    const char *ca_path;  /**< PEM CA for mqtts:// brokers (e.g. "/fs/a/MQTT_CA.PEM"); missing
                               file = built-in certificate bundle. */
    const char *broker_uri; /**< Broker URI; NULL or "" = CONFIG_CLU_MQTT_BROKER_URI (empty there = MQTT off). */
    const char *username;   /**< NULL or "" = CONFIG_CLU_MQTT_USERNAME (empty there = anonymous). */
    const char *password;   /**< NULL or "" = CONFIG_CLU_MQTT_PASSWORD. */
} clu_mqtt_cfg_t;

/**
 * @brief Register the Lua side (mqtt.lua). Call before clu_lua_start().
 * @param cfg Configuration.
 */
void clu_mqtt_init(const clu_mqtt_cfg_t *cfg);

/**
 * @brief UseCloud: connect when on (and a broker is configured), disconnect when off.
 * @param on Enable the connection.
 */
void clu_mqtt_set_enabled(bool on);

/**
 * @brief SYSTEM.Init (OM.LUA applied UseCloud): connect if online, UseCloud on and a broker is
 * configured. Idempotent; may come before clu_mqtt_init(). Home Assistant discovery waits for
 * SYSTEM.Init on the Lua side.
 */
void clu_mqtt_start(void);

/**
 * @brief Network state: true while some interface (WiFi / Ethernet) has link + IP. Online starts
 * the client without waiting for the Lua scripts; offline stops it, so no reconnect attempts
 * while there is no route. Non-blocking (a worker task does the start / stop); may come before
 * clu_mqtt_init().
 * @param online Some interface is up.
 */
void clu_mqtt_set_online(bool online);

/** @brief CloudConnection: broker connected. */
bool clu_mqtt_connected(void);

#ifdef __cplusplus
}
#endif
