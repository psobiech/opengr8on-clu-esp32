// Host stub: the ESP-IDF UART driver calls used by tfbus_uart.c, implemented by the test.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef int uart_port_t;
typedef enum { UART_DATA_8_BITS = 3 } uart_word_length_t;
typedef enum { UART_PARITY_DISABLE = 0 } uart_parity_t;
typedef enum { UART_STOP_BITS_1 = 1 } uart_stop_bits_t;
typedef enum { UART_HW_FLOWCTRL_DISABLE = 0 } uart_hw_flowcontrol_t;
typedef enum { UART_SCLK_DEFAULT = 0 } uart_sclk_t;
typedef enum { UART_MODE_UART = 0, UART_MODE_RS485_HALF_DUPLEX = 1 } uart_mode_t;
#define UART_PIN_NO_CHANGE (-1)

typedef struct {
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uart_sclk_t source_clk;
} uart_config_t;

esp_err_t uart_driver_install(uart_port_t n, int rx, int tx, int qsize, void *q, int flags);
esp_err_t uart_param_config(uart_port_t n, const uart_config_t *c);
esp_err_t uart_set_pin(uart_port_t n, int tx, int rx, int rts, int cts);
esp_err_t uart_set_mode(uart_port_t n, uart_mode_t mode);
esp_err_t uart_set_rx_timeout(uart_port_t n, uint8_t symbols);
esp_err_t uart_flush_input(uart_port_t n);
int uart_write_bytes(uart_port_t n, const void *src, size_t len);
esp_err_t uart_wait_tx_done(uart_port_t n, TickType_t ticks);
int uart_read_bytes(uart_port_t n, void *buf, uint32_t len, TickType_t ticks);
esp_err_t uart_get_buffered_data_len(uart_port_t n, size_t *size);
