/**
 * @file tfbus_uart.h
 * @brief RS485 half-duplex UART for TFBUS (115200 8N1, DE driven by UART RTS).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief UART configuration. */
typedef struct {
    int uart_num;   /**< UART port number; < 0 = no wire (only local endpoints, e.g. emulated modules). */
    int tx_pin;     /**< TX GPIO. */
    int rx_pin;     /**< RX GPIO. */
    int de_pin;     /**< RS485 driver-enable GPIO (UART RTS). */
    int baud;       /**< Baud rate. */
} tfbus_uart_cfg_t;

/**
 * @brief Install the UART driver in RS485 half-duplex mode.
 * @param cfg Configuration.
 * @return ESP_OK or an ESP-IDF error.
 */
esp_err_t tfbus_uart_init(const tfbus_uart_cfg_t *cfg);

/**
 * @brief Send one frame and collect the reply (first complete RX frame).
 *
 * Echo of own TX is discarded.
 *
 * @param tx         Frame to send.
 * @param txlen      Frame length.
 * @param rx         Reply buffer.
 * @param rxcap      Capacity of @p rx.
 * @param timeout_ms Reply timeout.
 * @return Reply length (0 = no reply), -1 on error.
 */
int tfbus_uart_transact(const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxcap, int timeout_ms);

/**
 * @brief Send without waiting for a reply (broadcasts).
 * @param tx    Frame to send.
 * @param txlen Frame length.
 * @return ESP_OK or an ESP-IDF error.
 */
esp_err_t tfbus_uart_send(const uint8_t *tx, size_t txlen);

/**
 * @brief Local bus endpoint: modules emulated by the firmware itself (e.g. on-board IO).
 *
 * Sees every frame before the wire, in the bus task. Returns the reply length written to @p rx
 * (> 0: frame handled locally, nothing goes on the wire), 0 if the frame is not (only) for a
 * local module (it goes on the wire as usual), or < 0 if a local module owns the frame but has
 * no reply (handled, the caller sees no reply). For frames sent with tfbus_uart_send() the
 * result is ignored and the frame always goes on the wire too.
 */
typedef int (*tfbus_uart_local_fn_t)(const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxcap);

/** @brief Max local endpoints. */
#define TFBUS_UART_MAX_LOCAL 4

/**
 * @brief Add a local bus endpoint; frames are offered to the endpoints in the order added, the
 * first that owns one answers it. Call before the bus master starts.
 * @param local_endpoint Endpoint.
 * @return false if @p local_endpoint is NULL or TFBUS_UART_MAX_LOCAL are installed.
 */
bool tfbus_uart_add_local(tfbus_uart_local_fn_t local_endpoint);

/**
 * @brief Replace all local endpoints with @p local_endpoint (NULL = none).
 * @param local_endpoint Endpoint.
 */
void tfbus_uart_set_local(tfbus_uart_local_fn_t local_endpoint);

#ifdef __cplusplus
}
#endif
