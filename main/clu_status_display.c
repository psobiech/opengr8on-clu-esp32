/**
 * @file clu_status_display.c
 * @brief Status screen on the board's OLED (CLU_DISPLAY), refreshed every second by its own task.
 *
 * Rows: CLU serial, Ethernet IP, WiFi IP (the enabled interfaces), local date and time, TF-Bus
 * modules online / declared, uptime. Only changed rows go over I2C (typically the clock and the
 * uptime), so the shared bus with the RTC and the on-board IO expander stays free.
 */
#include "clu_status_display.h"

#include "sdkconfig.h"

#if CONFIG_CLU_DISPLAY

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "clu_display.h"
#include "clu_net.h"
#include "clu_rtc.h"
#include "clu_state.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tfbus_master.h"

#define REFRESH_MS      1000
#define TASK_STACK_SIZE 3072
#define TASK_PRIORITY   1  // below the bus, network and Lua tasks
#define ROW_CAPACITY    32 // > CLU_DISPLAY_COLUMNS: the display cuts longer rows

typedef char status_rows_t[CLU_DISPLAY_ROWS][ROW_CAPACITY];

static void status_display_task(void *arg);
static void compose_rows(status_rows_t rows);
static void format_ip(char *row, const char *interface_name, uint32_t ip_be);
static void format_clock(char *row);
static void format_uptime(char *row);
#if CONFIG_CLU_TFBUS_MASTER
static void format_tfbus(char *row);
#endif

void clu_status_display_start(void)
{
    if (!clu_display_init(CONFIG_CLU_DISPLAY_SDA_GPIO, CONFIG_CLU_DISPLAY_SCL_GPIO, CONFIG_CLU_DISPLAY_ADDR)) {
        return;
    }
    xTaskCreate(status_display_task, "display", TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL);
}

/** @brief Task: compose and show the rows every REFRESH_MS. */
static void status_display_task(void *arg)
{
    static status_rows_t rows;
    const char *row_texts[CLU_DISPLAY_ROWS];
    for (int row = 0; row < CLU_DISPLAY_ROWS; row++) {
        row_texts[row] = rows[row];
    }
    for (;;) {
        compose_rows(rows);
        clu_display_show(row_texts);
        vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
    }
}

/** @brief Current status, one item per row; unused rows blank. */
static void compose_rows(status_rows_t rows)
{
    memset(rows, 0, sizeof(status_rows_t));
    int row = 0;
    snprintf(rows[row++], ROW_CAPACITY, "CLU %lu", (unsigned long)g_clu.sn);
#if CONFIG_CLU_ETH
    format_ip(rows[row++], "ETH", clu_net_ip(CLU_NET_ETH));
#endif
#if CONFIG_CLU_WIFI
    format_ip(rows[row++], "WiFi", clu_net_ip(CLU_NET_WIFI));
#endif
    row++; // blank separator
    format_clock(rows[row++]);
#if CONFIG_CLU_TFBUS_MASTER
    format_tfbus(rows[row++]);
#endif
    format_uptime(rows[row++]);
}

/** @brief "<name> <dotted IPv4>", or "<name> -" without an address. */
static void format_ip(char *row, const char *interface_name, uint32_t ip_be)
{
    if (!ip_be) {
        snprintf(row, ROW_CAPACITY, "%s -", interface_name);
        return;
    }
    const esp_ip4_addr_t address = {.addr = ip_be};
    char dotted_address[16];
    esp_ip4addr_ntoa(&address, dotted_address, sizeof dotted_address);
    snprintf(row, ROW_CAPACITY, "%s %s", interface_name, dotted_address);
}

/** @brief Local date and time (time zone from clu_time), or a note while the clock is not set. */
static void format_clock(char *row)
{
    const time_t now = time(NULL);
    struct tm local_time;
    if (now < CLU_RTC_MIN_VALID || !localtime_r(&now, &local_time)) {
        snprintf(row, ROW_CAPACITY, "Time not set");
        return;
    }
    strftime(row, ROW_CAPACITY, "%Y-%m-%d %H:%M:%S", &local_time);
}

#if CONFIG_CLU_TFBUS_MASTER
/**
 * @brief "TF-Bus <online>/<declared> online". online is a single word written by the bus task;
 * freed slots (refresh) are NULL (as in clu_sysinfo).
 */
static void format_tfbus(char *row)
{
    int module_count = 0, online_count = 0;
    for (int i = 0; i < tfbus_master_count(); i++) {
        const tfbus_module_t *module = tfbus_master_get(i);
        if (module) {
            module_count++;
            online_count += module->online;
        }
    }
    snprintf(row, ROW_CAPACITY, "TF-Bus %d/%d online", online_count, module_count);
}
#endif

/** @brief "Up <days>d hh:mm:ss" since boot. */
static void format_uptime(char *row)
{
    const uint64_t uptime_s = (uint64_t)esp_timer_get_time() / 1000000;
    snprintf(row, ROW_CAPACITY, "Up %lud %02u:%02u:%02u", (unsigned long)(uptime_s / 86400),
             (unsigned)(uptime_s / 3600 % 24), (unsigned)(uptime_s / 60 % 60), (unsigned)(uptime_s % 60));
}

#endif // CONFIG_CLU_DISPLAY
