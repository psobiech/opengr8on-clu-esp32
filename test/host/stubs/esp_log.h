#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
// Info/debug: arguments type-checked and "used" (sizeof does not evaluate), nothing printed.
#define ESP_LOGI(tag, fmt, ...) ((void)(tag), (void)sizeof(printf(fmt, ##__VA_ARGS__)))
#define ESP_LOGD(tag, fmt, ...) ((void)(tag), (void)sizeof(printf(fmt, ##__VA_ARGS__)))
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG } esp_log_level_t;
#define ESP_LOG_BUFFER_HEX_LEVEL(tag, buf, len, level) ((void)(tag), (void)(buf), (void)(len), (void)(level))
#include <stdarg.h>
#include <stdint.h>
static inline uint32_t esp_log_timestamp(void) { return 0; }
typedef int (*vprintf_like_t)(const char *, va_list);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t fn); // defined by the test that needs it
