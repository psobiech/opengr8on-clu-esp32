// Host tests for the RS485 transport (tfbus_uart.c) against a fake ESP-IDF UART driver: TX is
// recorded, RX bytes arrive on a simulated clock (esp_timer_get_time), so timeouts, echo and
// split replies can be scripted.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_timer.h"
#include "tfbus_frame.h"
#include "tfbus_uart.h"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- fake UART
static int64_t s_now_us;
int64_t esp_timer_get_time(void) { return s_now_us; }

static struct {
    int port, rxbuf, baud, tx, rx, rts, mode, rx_timeout;
} s_cfg;
static uint8_t s_tx[256];
static size_t s_txlen;
static int s_flushes;
static bool s_short_write, s_echo;

// RX bytes with their arrival time; s_rxpos = next unread byte.
static uint8_t s_rx[512];
static int64_t s_rx_at[512];
static size_t s_nrx, s_rxpos;

esp_err_t uart_driver_install(uart_port_t n, int rx, int tx, int qsize, void *q, int flags)
{
    s_cfg.port = n;
    s_cfg.rxbuf = rx;
    return ESP_OK;
}
esp_err_t uart_param_config(uart_port_t n, const uart_config_t *c)
{
    s_cfg.baud = c->baud_rate;
    return ESP_OK;
}
esp_err_t uart_set_pin(uart_port_t n, int tx, int rx, int rts, int cts)
{
    s_cfg.tx = tx, s_cfg.rx = rx, s_cfg.rts = rts;
    return ESP_OK;
}
esp_err_t uart_set_mode(uart_port_t n, uart_mode_t mode)
{
    s_cfg.mode = mode;
    return ESP_OK;
}
esp_err_t uart_set_rx_timeout(uart_port_t n, uint8_t symbols)
{
    s_cfg.rx_timeout = symbols;
    return ESP_OK;
}
esp_err_t uart_flush_input(uart_port_t n)
{
    s_flushes++;
    while (s_rxpos < s_nrx && s_rx_at[s_rxpos] <= s_now_us) {
        s_rxpos++; // stale bytes already received are dropped
    }
    return ESP_OK;
}

static void rx_at(int64_t delay_us, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_rx[s_nrx] = b[i];
        s_rx_at[s_nrx++] = s_now_us + delay_us;
    }
}

int uart_write_bytes(uart_port_t n, const void *src, size_t len)
{
    memcpy(s_tx + s_txlen, src, len);
    s_txlen += len;
    if (s_echo) {
        rx_at(100, src, len);
    }
    return s_short_write ? (int)len - 1 : (int)len;
}
esp_err_t uart_wait_tx_done(uart_port_t n, TickType_t ticks) { return ESP_OK; }

static size_t arrived(void)
{
    size_t k = s_rxpos;
    while (k < s_nrx && s_rx_at[k] <= s_now_us) {
        k++;
    }
    return k - s_rxpos;
}

esp_err_t uart_get_buffered_data_len(uart_port_t n, size_t *size)
{
    *size = arrived();
    return ESP_OK;
}

static int s_reads;
int uart_read_bytes(uart_port_t n, void *buf, uint32_t len, TickType_t ticks)
{
    s_reads++;
    if (!arrived()) {
        int64_t until = s_now_us + (int64_t)ticks * 1000;
        if (s_rxpos < s_nrx && s_rx_at[s_rxpos] <= until) {
            s_now_us = s_rx_at[s_rxpos]; // woken by the first byte
        } else {
            s_now_us = until;
            return 0;
        }
    }
    size_t a = arrived();
    size_t k = a < len ? a : len;
    memcpy(buf, s_rx + s_rxpos, k);
    s_rxpos += k;
    return (int)k;
}

static void reset(void)
{
    s_txlen = 0;
    s_nrx = s_rxpos = 0;
    s_echo = s_short_write = false;
}

// Module -> CLU frame: aa 00 len payload xor
static size_t reply(uint8_t *out, const uint8_t *pl, size_t n)
{
    out[0] = TFBUS_START;
    out[1] = TFBUS_CLU_ADDR;
    out[2] = (uint8_t)(n + 1);
    memcpy(out + 3, pl, n);
    out[3 + n] = tfbus_xor(out, 3 + n);
    return 4 + n;
}

int main(void)
{
    uint8_t f[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME], r[TFBUS_MAX_FRAME];
    size_t fl = tfbus_status_poll(f, sizeof f, 5);
    static const uint8_t pl[] = {0x03, 0x00, 0x01, 0x01, 0x01};
    size_t rl = reply(r, pl, sizeof pl);

    check("send before init -> ESP_ERR_INVALID_STATE", tfbus_uart_send(f, fl) == ESP_ERR_INVALID_STATE);
    check("transact before init -> -1", tfbus_uart_transact(f, fl, rx, sizeof rx, 20) == -1);

    tfbus_uart_cfg_t cfg = {.uart_num = 2, .tx_pin = 17, .rx_pin = 16, .de_pin = 4, .baud = 115200};
    check("init", tfbus_uart_init(&cfg) == ESP_OK);
    check("init: UART2 at 115200, pins tx/rx/DE(RTS), RS485 half duplex, 3-symbol RX timeout",
          s_cfg.port == 2 && s_cfg.baud == 115200 && s_cfg.tx == 17 && s_cfg.rx == 16 && s_cfg.rts == 4 &&
              s_cfg.mode == UART_MODE_RS485_HALF_DUPLEX && s_cfg.rx_timeout == 3 && s_cfg.rxbuf == 512);

    // reply 2 ms after the request, stale input before it is flushed
    reset();
    rx_at(0, (const uint8_t[]){0x55, 0x66}, 2);
    rx_at(2000, r, rl);
    int flushes = s_flushes;
    int64_t t0 = s_now_us;
    int n = tfbus_uart_transact(f, fl, rx, sizeof rx, 20);
    check("transact: request sent, stale input flushed",
          s_txlen == fl && memcmp(s_tx, f, fl) == 0 && s_flushes == flushes + 1);
    check("transact: reply returned as soon as complete (not after the timeout)",
          n == (int)rl && memcmp(rx, r, rl) == 0 && s_now_us - t0 == 2000);

    // echo of our own frame (RE tied low) is skipped
    reset();
    s_echo = true;
    rx_at(1500, r, rl);
    n = tfbus_uart_transact(f, fl, rx, sizeof rx, 20);
    check("transact: own TX echo skipped, reply returned", n == (int)rl && memcmp(rx, r, rl) == 0);

    // reply arriving byte by byte
    reset();
    for (size_t i = 0; i < rl; i++) {
        rx_at(1000 + 100 * (int64_t)i, r + i, 1);
    }
    n = tfbus_uart_transact(f, fl, rx, sizeof rx, 20);
    check("transact: reply split over many reads", n == (int)rl && memcmp(rx, r, rl) == 0);

    // two replies in one burst: the first one wins
    reset();
    uint8_t two[2 * TFBUS_MAX_FRAME];
    size_t tl = reply(two, pl, sizeof pl);
    tl += reply(two + tl, (const uint8_t[]){0x09}, 1);
    rx_at(1000, two, tl);
    n = tfbus_uart_transact(f, fl, rx, sizeof rx, 20);
    check("transact: first reply of a burst kept", n == (int)rl && memcmp(rx, r, rl) == 0);

    // more than one chunk buffered at once (64-byte reads): noise, then the reply
    reset();
    uint8_t big[100];
    memset(big, 0x11, sizeof big);
    rx_at(500, big, sizeof big);
    rx_at(500, r, rl);
    n = tfbus_uart_transact(f, fl, rx, sizeof rx, 20);
    check("transact: reply found after >64 bytes of noise", n == (int)rl && memcmp(rx, r, rl) == 0);

    // frame for another address (module -> module), then silence -> timeout
    reset();
    uint8_t other[TFBUS_MAX_FRAME];
    size_t ol = tfbus_status_poll(other, sizeof other, 7);
    rx_at(1000, other, ol);
    t0 = s_now_us;
    n = tfbus_uart_transact(f, fl, rx, sizeof rx, 20);
    check("transact: frames not addressed to the CLU ignored, timeout -> 0", n == 0 && s_now_us - t0 >= 20000);

    // reply larger than the caller's buffer
    reset();
    rx_at(1000, r, rl);
    n = tfbus_uart_transact(f, fl, rx, rl - 1, 20);
    check("transact: reply larger than rxcap ignored -> 0", n == 0);

    // no answer at all
    reset();
    t0 = s_now_us;
    n = tfbus_uart_transact(f, fl, rx, sizeof rx, 20);
    check("transact: silence -> 0 after the timeout", n == 0 && s_now_us - t0 == 20000);

    // broadcast that got garbled bytes back (collision): logged, 0
    reset();
    size_t dl = tfbus_discover(f, sizeof f);
    rx_at(1000, (const uint8_t[]){0xaa, 0x00, 0x08, 0x12, 0x34}, 5);
    n = tfbus_uart_transact(f, dl, rx, sizeof rx, 40);
    check("transact: garbled reply to a broadcast -> 0", n == 0);

    // UART write failure
    reset();
    s_short_write = true;
    check("send: short write -> ESP_FAIL", tfbus_uart_send(f, fl) == ESP_FAIL);
    check("transact: send failure -> -1", tfbus_uart_transact(f, fl, rx, sizeof rx, 20) == -1);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
