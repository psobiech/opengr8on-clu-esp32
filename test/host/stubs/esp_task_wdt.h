// Host stub: task watchdog (esp_system). Subscribing and feeding are no-ops on the host.
#pragma once
#include "esp_err.h"
#include "freertos/task.h"
static inline esp_err_t esp_task_wdt_add(TaskHandle_t task_handle) { (void)task_handle; return ESP_OK; }
static inline esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle) { (void)task_handle; return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
typedef void (*task_wdt_msg_handler)(void *opaque, const char *msg);
esp_err_t esp_task_wdt_print_triggered_tasks(task_wdt_msg_handler msg_handler, void *opaque, int *cpus_fail); // defined by the test
