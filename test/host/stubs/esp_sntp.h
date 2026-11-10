#pragma once
#include <stdbool.h> // the real header pulls it in via lwIP
#include <sys/time.h>
typedef enum { ESP_SNTP_OPMODE_POLL } esp_sntp_operatingmode_t;
void esp_sntp_stop(void);
void esp_sntp_setoperatingmode(esp_sntp_operatingmode_t m);
void esp_sntp_setservername(int idx, const char *server);
void sntp_set_time_sync_notification_cb(void (*cb)(struct timeval *tv));
void esp_sntp_init(void);
