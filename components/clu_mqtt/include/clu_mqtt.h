/**
 * @file clu_mqtt.h
 * @brief MQTT link + Home Assistant discovery (mqtt.lua).
 *
 * Broker settings come from menuconfig ("OpenGr8on ESP32 CLU" → "OpenGr8on MQTT / Home Assistant");
 * the local CLU feature UseCloud (18) switches the connection on/off and CloudConnection (19)
 * reports it.
 *
 * Threading: all functions are thread-safe (clu_mqtt_set_enabled() comes from the Lua task,
 * clu_mqtt_start() from the IP event and SYSTEM.Init).
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
    const char *ca_path;  /**< PEM CA for mqtts:// brokers (e.g. "/fs/a/MQTT_CA.PEM"); missing
                               file = built-in certificate bundle. */
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
 * @brief Connect (if UseCloud and a broker is configured). Idempotent; may come before clu_mqtt_init().
 *
 * Called when the network gets an IP, independent of the Lua scripts, and again from SYSTEM.Init
 * (OM.LUA applied UseCloud). Home Assistant discovery waits for SYSTEM.Init on the Lua side.
 */
void clu_mqtt_start(void);

/** @brief CloudConnection: broker connected. */
bool clu_mqtt_connected(void);

#ifdef __cplusplus
}
#endif
