#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum {
    ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO
} esp_reset_reason_t;
void esp_restart(void); // defined by the test
typedef void (*shutdown_handler_t)(void);
esp_err_t esp_register_shutdown_handler(shutdown_handler_t handler);
esp_reset_reason_t esp_reset_reason(void);
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);
