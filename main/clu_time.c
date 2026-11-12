/**
 * @file clu_time.c
 * @brief Wall clock: SNTP client, battery-backed RTC + CLU TimeZone feature (index 23) mapped to POSIX
 * TZ strings.
 *
 * The system clock is set from the RTC at boot (before Lua), so scripts have the right time without
 * the network when the RTC has a backup cell. Every SNTP sync and SetDateTime writes the RTC back.
 *
 * NTP server / time zone set from OM.LUA (CLUxxx:set(22, "...") / set(23, n)) are persisted in NVS,
 * so the local time is right already before OnInit on the next boot.
 */
#include "clu_time.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "clu_rtc.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "clu_time";

#define NVS_NS       "clu_time"
#define DEFAULT_NTP  "pool.ntp.org"

/** @brief POSIX TZ strings; index = CLU TimeZone enum (clu_ZWAVE_2 interface, feature 23). */
static const char *const TZ[] = {
    "CET-1CEST,M3.5.0,M10.5.0/3",         // 0  Europe/Warsaw
    "GMT0BST,M3.5.0/1,M10.5.0",           // 1  Europe/London
    "MSK-3",                              // 2  Europe/Moscow
    "<+03>-3",                            // 3  Europe/Istanbul
    "EET-2EEST,M3.5.0/3,M10.5.0/4",       // 4  Europe/Athens
    "<+04>-4",                            // 5  Asia/Dubai
    "WIB-7",                              // 6  Asia/Jakarta
    "HKT-8",                              // 7  Asia/HongKong
    "AEST-10AEDT,M10.1.0,M4.1.0/3",       // 8  Australia/Sydney
    "AWST-8",                             // 9  Australia/Perth
    "AEST-10",                            // 10 Australia/Brisbane
    "NZST-12NZDT,M9.5.0,M4.1.0/3",        // 11 NewZealand/Auckland
    "HST10",                              // 12 USA/Hawaii
    "AKST9AKDT,M3.2.0,M11.1.0",           // 13 USA/Alaska
    "CST6CDT,M3.2.0,M11.1.0",             // 14 USA/CentralTime
    "EST5EDT,M3.2.0,M11.1.0",             // 15 USA/EasternTime
    "AST4",                               // 16 USA/AtlanticTime
    "<-03>3",                             // 17 America/Brazil
    "<-05>5",                             // 18 America/Colombia
    "<-03>3",                             // 19 America/Argentina
    "CST6",                               // 20 America/CentralAmerica
    "PST8PDT,M3.2.0,M11.1.0",             // 21 Pacific Time
    "UTC0",                               // 22 UTC
    "IST-5:30",                           // 23 Asia/Kolkata
    "<+0330>-3:30",                       // 24 Asia/Tehran
    "<+03>-3",                            // 25 Asia/Riyadh
    "WAT-1",                              // 26 Africa/Lagos
    "SAST-2",                             // 27 Africa/Johannesburg
};

static char s_ntp[64] = DEFAULT_NTP; // esp_sntp keeps the pointer
static int s_zone;
static bool s_sntp_started;

/** @brief Persist a string (@p s != NULL) or an i32 (@p n) under @p key. */
static void nvs_put(const char *key, const char *s, int32_t n)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (s) {
        nvs_set_str(h, key, s);
    } else {
        nvs_set_i32(h, key, n);
    }
    nvs_commit(h);
    nvs_close(h);
}

/**
 * @brief Set the TZ environment for a TimeZone enum index (out of range = 0, Europe/Warsaw).
 */
static void apply_zone(int zone)
{
    if (zone < 0 || zone >= (int)(sizeof TZ / sizeof TZ[0])) {
        zone = 0;
    }
    s_zone = zone;
    setenv("TZ", TZ[zone], 1);
    tzset();
}

/** @brief SNTP sync notification: keep the RTC in step. Runs in the lwIP task. */
static void time_synced(struct timeval *tv)
{
    ESP_LOGI(TAG, "time synced from %s", s_ntp);
#if CONFIG_CLU_RTC
    clu_rtc_set(tv ? tv->tv_sec : time(NULL));
#endif
}

/**
 * @brief Set the system clock from the RTC, or seed the RTC from a system clock that survived a
 * software reset (ESP-IDF keeps it in RTC memory) when the RTC itself lost power.
 */
static void rtc_restore(void)
{
#if CONFIG_CLU_RTC
#if CONFIG_CLU_RTC_DS3231
    const clu_rtc_chip_t chip = CLU_RTC_DS3231;
#else
    const clu_rtc_chip_t chip = CLU_RTC_PCF85063;
#endif
    if (!clu_rtc_init(chip, CONFIG_CLU_RTC_SDA_GPIO, CONFIG_CLU_RTC_SCL_GPIO)) {
        return;
    }
    time_t t;
    if (clu_rtc_get(&t)) {
        struct timeval tv = {.tv_sec = t};
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "clock set from RTC: %lld", (long long)t);
    } else if ((t = time(NULL)) >= CLU_RTC_MIN_VALID) {
        clu_rtc_set(t);
        ESP_LOGW(TAG, "RTC time invalid, seeded from the system clock kept across the reset");
    } else {
        ESP_LOGW(TAG, "RTC time invalid (no backup cell?): clock wrong until SNTP or SetDateTime");
    }
#endif
}

/** @brief (Re)start SNTP in poll mode with the current server. */
static void ntp_restart(void)
{
    if (s_sntp_started) {
        esp_sntp_stop();
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, s_ntp);
    sntp_set_time_sync_notification_cb(time_synced);
    esp_sntp_init();
    s_sntp_started = true;
}

void clu_time_init(void)
{
    nvs_handle_t h;
    int32_t zone = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof s_ntp;
        if (nvs_get_str(h, "ntp", s_ntp, &l) != ESP_OK || !s_ntp[0]) {
            strlcpy(s_ntp, DEFAULT_NTP, sizeof s_ntp);
        }
        nvs_get_i32(h, "tz", &zone);
        nvs_close(h);
    }
    apply_zone(zone);
    rtc_restore();
}

void clu_time_store(void)
{
#if CONFIG_CLU_RTC
    clu_rtc_set(time(NULL));
#endif
}

void clu_time_start_sntp(void)
{
    ntp_restart();
}

void clu_time_set_ntp_server(const char *server)
{
    if (!server || !server[0] || strcmp(server, s_ntp) == 0) {
        return;
    }
    strlcpy(s_ntp, server, sizeof s_ntp);
    nvs_put("ntp", s_ntp, 0);
    ESP_LOGI(TAG, "NTP server %s", s_ntp);
    if (s_sntp_started) {
        ntp_restart();
    }
}

void clu_time_set_zone(int zone)
{
    if (zone == s_zone) {
        return;
    }
    apply_zone(zone);
    nvs_put("tz", NULL, s_zone);
    ESP_LOGI(TAG, "time zone %d (%s)", s_zone, TZ[s_zone]);
}
