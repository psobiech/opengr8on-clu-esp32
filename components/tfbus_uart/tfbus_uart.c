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
static tfbus_uart_local_fn_t s_local;

void tfbus_uart_set_local(tfbus_uart_local_fn_t fn)
{
    s_local = fn;
}

esp_err_t tfbus_uart_init(const tfbus_uart_cfg_t *cfg)
{
    const uart_config_t uc = {
        .baud_rate = cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(cfg->uart_num, 512, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(cfg->uart_num, &uc));
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
static esp_err_t wire_send(const uint8_t *tx, size_t txlen)
{
    if (s_uart < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uart_flush_input(s_uart);
    if (uart_write_bytes(s_uart, tx, txlen) != (int)txlen) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(s_uart, pdMS_TO_TICKS(20));
}

esp_err_t tfbus_uart_send(const uint8_t *tx, size_t txlen)
{
    if (s_local) {
        uint8_t rx[TFBUS_MAX_FRAME];
        s_local(tx, txlen, rx, sizeof rx); // e.g. RESET broadcast: local modules hear it too
    }
    return wire_send(tx, txlen);
}

/** @brief Reply collector state for tfbus_uart_transact(). */
typedef struct {
    const uint8_t *tx;
    size_t txlen;
    uint8_t *rx;
    size_t rxcap;
    int got;
} collect_t;

/**
 * @brief RX parser callback: keep the first module -> CLU frame that is not our own echo.
 * @param f   Complete frame.
 * @param len Frame length.
 * @param ctx ::collect_t.
 */
static void on_frame(const uint8_t *f, size_t len, void *ctx)
{
    collect_t *c = ctx;
    if (c->got > 0) {
        return;
    }
    // Some transceivers echo our own TX (RE tied low) — skip it.
    if (len == c->txlen && memcmp(f, c->tx, len) == 0) {
        return;
    }
    if (f[1] != TFBUS_CLU_ADDR || len > c->rxcap) {
        return;
    }
    memcpy(c->rx, f, len);
    c->got = (int)len;
}

int tfbus_uart_transact(const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxcap, int timeout_ms)
{
    if (s_local) {
        int r = s_local(tx, txlen, rx, rxcap);
        if (r != 0) {
            return r > 0 ? r : 0;
        }
    }
    if (wire_send(tx, txlen) != ESP_OK) {
        return -1;
    }
    tfbus_rx_t parser;
    tfbus_rx_init(&parser);
    collect_t c = {.tx = tx, .txlen = txlen, .rx = rx, .rxcap = rxcap};
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint8_t chunk[64];
    uint8_t raw[24];
    size_t nraw = 0, total = 0;
    while (c.got == 0) {
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) {
            break;
        }
        // uart_read_bytes() returns only when `length` bytes arrived or the timeout expired: wait
        // for one byte, then take whatever else is already buffered, so a reply is parsed as
        // soon as it is complete instead of after the full timeout.
        TickType_t ticks = pdMS_TO_TICKS(left_us / 1000);
        int n = uart_read_bytes(s_uart, chunk, 1, ticks ? ticks : 1);
        if (n <= 0) {
            continue;
        }
        size_t more = 0;
        if (uart_get_buffered_data_len(s_uart, &more) == ESP_OK && more > 0) {
            if (more > sizeof chunk - 1) {
                more = sizeof chunk - 1;
            }
            int m = uart_read_bytes(s_uart, chunk + 1, more, 0);
            if (m > 0) {
                n += m;
            }
        }
        for (int i = 0; i < n && nraw < sizeof raw; i++) {
            raw[nraw++] = chunk[i];
        }
        total += (size_t)n;
        tfbus_rx_push(&parser, chunk, (size_t)n, on_frame, &c);
    }
    // Discovery diagnostics: a broadcast got bytes back that do not parse (collision / turnaround).
    if (c.got == 0 && total > 0 && txlen >= 3 && tx[1] == TFBUS_BROADCAST) {
        char hex[3 * sizeof raw + 1] = "";
        for (size_t i = 0; i < nraw; i++) {
            snprintf(hex + 3 * i, 4, "%02x ", raw[i]);
        }
        ESP_LOGW(TAG, "bcast type 0x%02x: no reply parsed, %u byte(s) received: %s", tx[2],
                 (unsigned)total, hex);
    }
    return c.got;
}
