/**
 * @file tfbus_uart.h
 * @brief RS485 half-duplex UART for TFBUS (115200 8N1, DE driven by UART RTS).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "tfbus_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief UART configuration. */
typedef struct {
    int uart_num;   /**< UART port number; < 0 = no wire (only local endpoints, e.g. emulated modules). */
    int tx_pin;     /**< TX GPIO. */
    int rx_pin;     /**< RX GPIO. */
    int de_pin;     /**< RS485 driver-enable GPIO (UART RTS); < 0 = auto-direction transceiver (plain UART mode). */
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
 * @param request_frame         Frame to send.
 * @param request_length      Frame length.
 * @param reply_buffer         Reply buffer.
 * @param reply_capacity      Capacity of @p reply_buffer.
 * @param timeout_ms Reply timeout.
 * @return Reply length (0 = no reply), -1 on error.
 */
int tfbus_uart_transact(const uint8_t *request_frame, size_t request_length, uint8_t *reply_buffer, size_t reply_capacity, int timeout_ms);

/**
 * @brief Whether the last tfbus_uart_transact() was owned by a local endpoint (a module emulated
 * by the firmware) instead of going on the wire. Same task as tfbus_uart_transact().
 * @return true if a local endpoint answered it or owned it without a reply.
 */
bool tfbus_uart_last_local(void);

/** @brief One module reply kept by tfbus_uart_collect(). */
typedef struct {
    uint8_t frame[TFBUS_MAX_FRAME]; /**< Reply frame. */
    size_t  length;                 /**< Frame length. */
} tfbus_uart_reply_t;

/**
 * @brief Send one frame and collect every reply until @p timeout_ms (DISCOVER).
 *
 * The real CLU keeps a DISCOVER open ~1 s and addresses every module that answered in it; modules
 * avoid colliding only by answering at different times. Local endpoints come first: if any of them
 * replies, those replies are returned at once and nothing goes on the wire (the wire modules
 * answer a later DISCOVER, once the local ones are addressed).
 *
 * @param request_frame  Frame to send.
 * @param request_length Frame length.
 * @param replies        Reply slots; replies beyond @p max_replies are dropped.
 * @param max_replies    Number of @p replies slots.
 * @param timeout_ms     How long to listen.
 * @param garbled        Set when received bytes did not form frames (colliding replies).
 * @return Replies kept (0..@p max_replies), -1 on error.
 */
int tfbus_uart_collect(const uint8_t *request_frame, size_t request_length, tfbus_uart_reply_t *replies, int max_replies, int timeout_ms, bool *garbled);

/**
 * @brief Send without waiting for a reply (broadcasts).
 * @param request_frame    Frame to send.
 * @param request_length Frame length.
 * @return ESP_OK or an ESP-IDF error.
 */
esp_err_t tfbus_uart_send(const uint8_t *request_frame, size_t request_length);

/** @brief Bus utilization over the last load window (tfbus_uart_load()), in permille. */
typedef struct {
    uint16_t wire_permille; /**< Bytes on the wire, both directions (10 bit times each at the baud rate). */
    uint16_t busy_permille; /**< Time the master held the bus: sending, then waiting for and receiving the
                                 replies, unanswered polls' timeouts included. The rest is idle gaps. */
} tfbus_uart_load_t;

/**
 * @brief Bus utilization of the last complete 10 s window (wire transfers only; local endpoints
 * do not use the bus). A bus left without traffic for two windows reports 0.
 *
 * Safe from any task. The two values may come from adjacent windows.
 * @param load Filled on success.
 * @return false if there is no wire or no window has completed yet.
 */
bool tfbus_uart_load(tfbus_uart_load_t *load);

/**
 * @brief Local bus endpoint: modules emulated by the firmware itself (e.g. on-board IO).
 *
 * Sees every frame before the wire, in the bus task. Returns the reply length written to @p reply_buffer
 * (> 0: frame handled locally, nothing goes on the wire), 0 if the frame is not (only) for a
 * local module (it goes on the wire as usual), or < 0 if a local module owns the frame but has
 * no reply (handled, the caller sees no reply). For frames sent with tfbus_uart_send() the
 * result is ignored and the frame always goes on the wire too.
 */
typedef int (*tfbus_uart_local_fn_t)(const uint8_t *request_frame, size_t request_length, uint8_t *reply_buffer, size_t reply_capacity);

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
