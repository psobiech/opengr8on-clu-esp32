/**
 * @file tfbus_uart.h
 * @brief RS485 half-duplex UART for TFBUS (115200 8N1, DE driven by UART RTS).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief UART configuration. */
typedef struct {
    int uart_num;   /**< UART port number. */
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

#ifdef __cplusplus
}
#endif
