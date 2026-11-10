/**
 * @file clu_telnet.h
 * @brief Log console over telnet (TCP, read-only).
 *
 * Every ESP_LOG line (incl. Lua print/Debug) is still printed on UART and also kept in a RAM ring
 * buffer, streamed to connected telnet/nc clients. A new client first gets the buffered history.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** @brief Telnet console configuration. */
typedef struct {
    uint16_t port;        /**< TCP port (23). */
    int max_clients;      /**< Concurrent clients. */
    int history;          /**< Ring buffer bytes. */
    const char *allow;    /**< "", "a.b.c.d" or "a.b.c.d/nn": peers allowed to connect. */
} clu_telnet_cfg_t;

/**
 * @brief Install the log hook; logs from here on are captured.
 *
 * Call early, before the network.
 *
 * @param cfg Configuration (copied; the `allow` string must stay valid).
 */
void clu_telnet_init(const clu_telnet_cfg_t *cfg);

/** @brief Start the TCP server task (after the netif is up). */
void clu_telnet_start(void);
