#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef const char *esp_event_base_t;
#ifndef ESP_EVENT_ANY_ID
#define ESP_EVENT_ANY_ID (-1)
#endif
typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base, int32_t id, void *data);
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t fn, void *arg);
