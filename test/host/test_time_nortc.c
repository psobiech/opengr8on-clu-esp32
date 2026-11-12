// Host test for clu_time.c built with CLU_RTC off: the RTC driver is never touched (these stubs
// count any call) while time zone and SNTP keep working.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "clu_rtc.h"
#include "clu_time.h"
#include "esp_sntp.h"
#include "fake_nvs.h"

static int failures, s_rtc_calls;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

bool clu_rtc_init(int sda, int scl) { s_rtc_calls++; return true; }
bool clu_rtc_get(time_t *t) { s_rtc_calls++; *t = 1750000000; return true; }
bool clu_rtc_set(time_t t) { s_rtc_calls++; return true; }

static int s_settimes;
int host_settimeofday(const struct timeval *tv, const void *tz) { s_settimes++; return 0; }
time_t host_time(time_t *t) { if (t) *t = 1760000000; return 1760000000; }
const char *host_cfg_serial = "", *host_cfg_pin = "", *host_cfg_iv = "";
FILE *host_fopen(const char *path, const char *mode) { return NULL; }
int host_mkdir(const char *path, mode_t mode) { return -1; }
int host_stat(const char *path, struct stat *st) { return -1; }

static void (*s_sntp_cb)(struct timeval *);
void esp_sntp_stop(void) {}
void esp_sntp_setoperatingmode(esp_sntp_operatingmode_t m) {}
void esp_sntp_setservername(int idx, const char *server) {}
void sntp_set_time_sync_notification_cb(void (*cb)(struct timeval *tv)) { s_sntp_cb = cb; }
void esp_sntp_init(void) {}

int main(void)
{
    nvs_reset();
    clu_time_init();
    check("CLU_RTC off: boot does not touch the RTC or the clock", s_rtc_calls == 0 && s_settimes == 0);
    check("CLU_RTC off: nothing stored at boot", !nvs_peek("clu_time", "tz"));
    check("CLU_RTC off: time zone still applied", getenv("TZ") && !strcmp(getenv("TZ"), "CET-1CEST,M3.5.0,M10.5.0/3"));
    clu_time_start_sntp();
    struct timeval tv = {.tv_sec = 1770000000};
    s_sntp_cb(&tv);
    clu_time_store();
    check("CLU_RTC off: SNTP sync and clu_time_store do not write an RTC", s_rtc_calls == 0);
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
