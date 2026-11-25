// Host stub: task watchdog (esp_system). Subscribing is a no-op on the host; a test sets
// host_task_wdt_subscribed to model a caller subscribed to the watchdog and counts its check-ins.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/task.h"
static bool host_task_wdt_subscribed;
static int host_task_wdt_resets;
static inline esp_err_t esp_task_wdt_add(TaskHandle_t task_handle) { (void)task_handle; return ESP_OK; }
static inline esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle) { (void)task_handle; return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void)
{
    host_task_wdt_resets++;
    return ESP_OK;
}
static inline esp_err_t esp_task_wdt_status(TaskHandle_t task_handle)
{
    (void)task_handle;
    return host_task_wdt_subscribed ? ESP_OK : ESP_ERR_NOT_FOUND;
}
typedef void (*task_wdt_msg_handler)(void *opaque, const char *msg);
esp_err_t esp_task_wdt_print_triggered_tasks(task_wdt_msg_handler msg_handler, void *opaque, int *cpus_fail); // defined by the test
