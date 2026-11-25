/**
 * @file tfbus_uart.c
 * @brief RS485 half-duplex UART transport for TFBUS (ESP-IDF UART driver in RS485 mode).
 */
#include "tfbus_uart.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "tfbus_frame.h"

static const char *TAG = "tfbus_uart";
static int s_uart = -1;
static bool s_nowire; // initialised without a UART: only local endpoints answer
static tfbus_uart_local_fn_t s_local[TFBUS_UART_MAX_LOCAL];
static int s_nlocal;

void tfbus_uart_set_local(tfbus_uart_local_fn_t local_endpoint)
{
    s_nlocal = 0;
    tfbus_uart_add_local(local_endpoint);
}

bool tfbus_uart_add_local(tfbus_uart_local_fn_t local_endpoint)
{
    if (!local_endpoint || s_nlocal == TFBUS_UART_MAX_LOCAL) {
        return false;
    }
    s_local[s_nlocal++] = local_endpoint;
    return true;
}

/** @brief Offer a frame to the local endpoints in turn: the first that owns it answers. */
static int local(const uint8_t *request_frame, size_t request_length, uint8_t *reply_buffer, size_t reply_capacity)
{
    for (int i = 0; i < s_nlocal; i++) {
        int reply_length = s_local[i](request_frame, request_length, reply_buffer, reply_capacity);
        if (reply_length != 0) {
            return reply_length;
        }
    }
    return 0;
}

esp_err_t tfbus_uart_init(const tfbus_uart_cfg_t *cfg)
{
    if (cfg->uart_num < 0) {
        s_nowire = true;
        ESP_LOGI(TAG, "no RS485 wire: emulated modules only");
        return ESP_OK;
    }
    const uart_config_t uart_config = {
        .baud_rate = cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(cfg->uart_num, 512, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(cfg->uart_num, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, cfg->de_pin, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(cfg->uart_num, UART_MODE_RS485_HALF_DUPLEX));
    // ~3 byte times at 115200 -> frame boundary detection
    ESP_ERROR_CHECK(uart_set_rx_timeout(cfg->uart_num, 3));
    s_uart = cfg->uart_num;
    ESP_LOGI(TAG, "RS485 on UART%d tx=%d rx=%d de=%d @%d", cfg->uart_num, cfg->tx_pin,
             cfg->rx_pin, cfg->de_pin, cfg->baud);
    return ESP_OK;
}

/** @brief Put a frame on the wire. */
static esp_err_t wire_send(const uint8_t *request_frame, size_t request_length)
{
    if (s_nowire) {
        return ESP_OK; // nobody out there
    }
    if (s_uart < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uart_flush_input(s_uart);
    if (uart_write_bytes(s_uart, request_frame, request_length) != (int)request_length) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(s_uart, pdMS_TO_TICKS(20));
}

esp_err_t tfbus_uart_send(const uint8_t *request_frame, size_t request_length)
{
    for (int i = 0; i < s_nlocal; i++) {
        uint8_t reply_buffer[TFBUS_MAX_FRAME];
        s_local[i](request_frame, request_length, reply_buffer, sizeof reply_buffer); // e.g. RESET broadcast: local modules hear it too
    }
    return wire_send(request_frame, request_length);
}

/** @brief Reply collector state for tfbus_uart_transact() / tfbus_uart_collect(). */
typedef struct {
    const uint8_t *request_frame;
    size_t request_length;
    tfbus_uart_reply_t *replies;
    int max_replies;
    size_t reply_capacity; /**< Largest reply kept. */
    bool listen_full_window; /**< Keep listening after the first reply (collect). */
    int got;               /**< Replies kept. */
    size_t framed_bytes;   /**< Received bytes that formed frames (any frame, kept or not). */
} collect_t;

static int receive(collect_t *collector, int timeout_ms);
static void on_frame(const uint8_t *frame, size_t len, void *ctx);

int tfbus_uart_transact(const uint8_t *request_frame, size_t request_length, uint8_t *reply_buffer, size_t reply_capacity, int timeout_ms)
{
    int local_reply_length = local(request_frame, request_length, reply_buffer, reply_capacity);
    if (local_reply_length != 0) {
        return local_reply_length > 0 ? local_reply_length : 0;
    }
    if (s_nowire) {
        return 0; // no module on a wire answers
    }
    if (wire_send(request_frame, request_length) != ESP_OK) {
        return -1;
    }
    tfbus_uart_reply_t reply;
    collect_t collector = {.request_frame = request_frame, .request_length = request_length, .replies = &reply,
                           .max_replies = 1,
                           .reply_capacity = reply_capacity < sizeof reply.frame ? reply_capacity : sizeof reply.frame};
    receive(&collector, timeout_ms);
    if (collector.got == 0) {
        return 0;
    }
    memcpy(reply_buffer, reply.frame, reply.length);
    return (int)reply.length;
}

int tfbus_uart_collect(const uint8_t *request_frame, size_t request_length, tfbus_uart_reply_t *replies, int max_replies, int timeout_ms, bool *garbled)
{
    *garbled = false;
    int reply_count = 0;
    for (int i = 0; i < s_nlocal && reply_count < max_replies; i++) {
        int reply_length = s_local[i](request_frame, request_length, replies[reply_count].frame, sizeof replies[reply_count].frame);
        if (reply_length > 0) {
            replies[reply_count++].length = (size_t)reply_length;
        }
    }
    if (reply_count > 0 || s_nowire) {
        return reply_count; // local modules first (no 1 s wire window each), the wire after them
    }
    if (wire_send(request_frame, request_length) != ESP_OK) {
        return -1;
    }
    collect_t collector = {.request_frame = request_frame, .request_length = request_length,
                           .replies = replies, .max_replies = max_replies,
                           .reply_capacity = sizeof replies->frame, .listen_full_window = true};
    size_t received = (size_t)receive(&collector, timeout_ms);
    *garbled = received > collector.framed_bytes;
    return collector.got;
}

/**
 * @brief Read the wire until the first kept reply (or, with listen_full_window, until the timeout).
 * @return Bytes received.
 */
static int receive(collect_t *collector, int timeout_ms)
{
    tfbus_rx_t parser;
    tfbus_rx_init(&parser);
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint8_t chunk[64];
    uint8_t raw[24];
    size_t nraw = 0, total = 0;
    while (collector->listen_full_window || collector->got == 0) {
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) {
            break;
        }
        // uart_read_bytes() returns only when `length` bytes arrived or the timeout expired: wait
        // for one byte, then take whatever else is already buffered, so a reply is parsed as
        // soon as it is complete instead of after the full timeout.
        TickType_t ticks = pdMS_TO_TICKS(left_us / 1000);
        int bytes_read = uart_read_bytes(s_uart, chunk, 1, ticks ? ticks : 1);
        if (bytes_read <= 0) {
            continue;
        }
        size_t more = 0;
        if (uart_get_buffered_data_len(s_uart, &more) == ESP_OK && more > 0) {
            if (more > sizeof chunk - 1) {
                more = sizeof chunk - 1;
            }
            int extra_bytes_read = uart_read_bytes(s_uart, chunk + 1, more, 0);
            if (extra_bytes_read > 0) {
                bytes_read += extra_bytes_read;
            }
        }
        for (int i = 0; i < bytes_read && nraw < sizeof raw; i++) {
            raw[nraw++] = chunk[i];
        }
        total += (size_t)bytes_read;
        tfbus_rx_push(&parser, chunk, (size_t)bytes_read, on_frame, collector);
    }
    // Discovery diagnostics: a broadcast got bytes back that do not parse (collision / turnaround).
    const uint8_t *request_frame = collector->request_frame;
    if (collector->got == 0 && total > 0 && collector->request_length >= 3 && request_frame[1] == TFBUS_BROADCAST) {
        char hex[3 * sizeof raw + 1] = "";
        for (size_t i = 0; i < nraw; i++) {
            snprintf(hex + 3 * i, 4, "%02x ", raw[i]);
        }
        ESP_LOGW(TAG, "bcast type 0x%02x: no reply parsed, %u byte(s) received: %s", request_frame[2],
                 (unsigned)total, hex);
    }
    return (int)total;
}

/**
 * @brief RX parser callback: keep module -> CLU frames.
 *
 * The UART drops our own TX in RS485 half-duplex mode (ESP-IDF clears rs485tx_rx_en), so no echo
 * reaches the parser.
 * @param frame Complete frame.
 * @param len   Frame length.
 * @param ctx   ::collect_t.
 */
static void on_frame(const uint8_t *frame, size_t len, void *ctx)
{
    collect_t *collector = ctx;
    collector->framed_bytes += len;
    if (collector->got >= collector->max_replies) {
        return;
    }
    if (frame[1] != TFBUS_CLU_ADDR || len > collector->reply_capacity) {
        return;
    }
    tfbus_uart_reply_t *reply = &collector->replies[collector->got++];
    memcpy(reply->frame, frame, len);
    reply->length = len;
}
