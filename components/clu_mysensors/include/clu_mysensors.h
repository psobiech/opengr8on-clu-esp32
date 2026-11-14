/**
 * @file clu_mysensors.h
 * @brief MySensors gateway on the RS485 bus (replaces the TF-Bus master, menuconfig CLU_MYSENSORS).
 *
 * The CLU is node 0, the gateway, on a MySensors RS485 network (nodes built with MY_RS485; frames
 * and binary messages in ms_rs485.h). It answers what a gateway answers itself (find parent,
 * ping, registration, echo) and is the controller for the rest:
 *   - I_ID_REQUEST: hands out the lowest free node id (1..254), kept in NVS so ids survive reboots
 *   - I_CONFIG: metric / imperial; I_TIME: local time as seconds since 1970
 *   - C_REQ: answered from the last value set (by the node or by a script), if known
 *   - presentation, sketch name / version, battery level: kept per node (MySensors.nodes())
 * Frames go out when the bus has been quiet for a few ms. Every 30 s, if bytes came in, the bus is
 * judged (valid frames / garbage with a byte dump), changes logged.
 * Binary sensors and outputs can also appear in OM as channels of emulated IO_MODULE_DIN_8
 * modules (ms_vmod.h). Every message (but log lines) also goes to the scripts: mysensors.lua,
 * global `MySensors` (USER.LUA API documented there).
 *
 * Threading: one task owns the UART. Scripts send through a queue; the node registry and value
 * cache are shared under a mutex.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "ms_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Configuration. */
typedef struct {
    int uart_num;      /**< UART port. */
    int tx_pin;        /**< TX GPIO. */
    int rx_pin;        /**< RX GPIO. */
    int de_pin;        /**< RS485 driver enable (UART RTS); -1 = auto-direction transceiver. */
    int baud;          /**< The nodes' MY_RS485_BAUD_RATE. */
    bool metric;       /**< I_CONFIG answer: "M" (true) or "I". */
    bool trace;        /**< Log every message in both directions at INFO (MySensors.trace() at runtime). */
    int max_modules;   /**< Max emulated IO_MODULE_DIN_8 modules, created as binary sensors /
                            outputs present themselves (0..8, 0 = none); needs the TF-Bus master
                            running (without a wire). */
} clu_mysensors_cfg_t;

/**
 * @brief Open the UART, load the node ids from NVS, register the Lua side and start the task.
 *
 * Call after nvs_flash_init(), before clu_lua_start().
 *
 * @param cfg Configuration.
 * @return ESP_OK, or an error if the UART / task could not be set up.
 */
esp_err_t clu_mysensors_init(const clu_mysensors_cfg_t *cfg);

/**
 * @brief Queue a message to the gateway. Thread-safe, never blocks.
 * @param message Message (payload at most MS_MAX_PAYLOAD chars).
 * @return false if the payload is too long or the queue is full.
 */
bool clu_mysensors_send(const ms_msg_t *message);

/**
 * @brief Discovery hook (tfbus_master_on_refresh(), bus task, before the RESET): the emulated
 * modules of nodes not heard since the last node check are removed (MySensors nodes that are gone,
 * changed id, ...), then a new check starts (every known node id asked to present itself). Nothing is
 * removed while a check is still running.
 */
void clu_mysensors_refresh(void);

/** @brief The gateway runs (the CLU is the gateway: always, once initialised). */
bool clu_mysensors_connected(void);

#ifdef __cplusplus
}
#endif
