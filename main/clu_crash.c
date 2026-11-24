/**
 * @file clu_crash.c
 * @brief What crashed the previous boot: a short record kept in RTC memory across the reset.
 *
 * __wrap_esp_panic_handler() runs first in every panic (exception, abort / assert, interrupt or
 * task watchdog) and fills the record before ESP-IDF's handler prints the backtrace and resets.
 * The task watchdog's ISR hook runs just before its panic and names the tasks that stopped checking
 * in (the panic itself only sees whichever task was running).
 *
 * RTC_NOINIT memory survives the software reset of a panic, not a power cut. A core dump would
 * need a flash partition big enough for the Lua task's 48 KB stack, which the existing partition
 * tables cannot fit without moving the FAT storage.
 */
#include "clu_crash.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "crash";

#define RECORD_MAGIC 0x43525348u // "CRSH": the record was written by this firmware's panic handler

/** @brief Crash record in RTC memory: written in the panic, read by the next boot. */
typedef struct {
    uint32_t magic;
    uint32_t program_counter;
    char task_name[configMAX_TASK_NAME_LEN];
    char cause[96];
} crash_record_t;

static RTC_NOINIT_ATTR crash_record_t s_record;
static char s_watchdog_tasks[64]; // filled by the task watchdog ISR right before its panic
static char s_last_crash[192];    // the previous boot's crash, formatted by clu_crash_init()

// ESP-IDF's own esp_panic_handler, reached through -Wl,--wrap. g_panic_abort / g_panic_abort_details
// (panic_internal.h) are set by abort(), assert and the task watchdog.
void __real_esp_panic_handler(panic_info_t *info);

static void collect_watchdog_task(void *opaque, const char *message);
static void append_text(char *buffer, size_t capacity, const char *text);

void clu_crash_init(void)
{
    esp_reset_reason_t reset_reason = esp_reset_reason();
    bool crashed = reset_reason == ESP_RST_PANIC || reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_INT_WDT;
    if (crashed && s_record.magic == RECORD_MAGIC) {
        // RTC memory: never trust the terminators.
        s_record.cause[sizeof s_record.cause - 1] = '\0';
        s_record.task_name[sizeof s_record.task_name - 1] = '\0';
        snprintf(s_last_crash, sizeof s_last_crash, "%s | task %s | pc 0x%08lx", s_record.cause, s_record.task_name,
                 (unsigned long)s_record.program_counter);
        ESP_LOGE(TAG, "previous boot crashed: %s", s_last_crash);
    }
    s_record.magic = 0;
}

const char *clu_crash_last(void)
{
    return s_last_crash[0] ? s_last_crash : NULL;
}

/** @brief Task watchdog ISR hook (weak in ESP-IDF): remember which tasks stopped checking in. */
void esp_task_wdt_isr_user_handler(void)
{
    int message_count = 0;
    s_watchdog_tasks[0] = '\0';
    esp_task_wdt_print_triggered_tasks(collect_watchdog_task, &message_count, NULL);
}

/** @brief Runs instead of esp_panic_handler (-Wl,--wrap): record the crash, then the IDF handler. */
void __wrap_esp_panic_handler(panic_info_t *info)
{
    s_record.cause[0] = '\0';
    if (s_watchdog_tasks[0]) {
        append_text(s_record.cause, sizeof s_record.cause, "task watchdog: ");
        append_text(s_record.cause, sizeof s_record.cause, s_watchdog_tasks);
    } else if (g_panic_abort) {
        append_text(s_record.cause, sizeof s_record.cause, g_panic_abort_details ? g_panic_abort_details : "abort()");
    } else {
        append_text(s_record.cause, sizeof s_record.cause, info->reason ? info->reason : "panic");
    }
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle(); // NULL before the scheduler runs
    s_record.task_name[0] = '\0';
    append_text(s_record.task_name, sizeof s_record.task_name, current_task ? pcTaskGetName(current_task) : "-");
    s_record.program_counter = (uint32_t)(uintptr_t)info->addr;
    s_record.magic = RECORD_MAGIC;
    __real_esp_panic_handler(info);
}

/**
 * @brief esp_task_wdt_print_triggered_tasks() line handler: a caption, then "\n - ", name and
 *        " (CPU n)" per task. Builds "clu_lua (CPU 1), tfbus (CPU 1)".
 */
static void collect_watchdog_task(void *opaque, const char *message)
{
    int *message_count = opaque;
    if ((*message_count)++ == 0) {
        return; // caption
    }
    if (strcmp(message, "\n - ") == 0) {
        message = s_watchdog_tasks[0] ? ", " : "";
    }
    append_text(s_watchdog_tasks, sizeof s_watchdog_tasks, message);
}

/** @brief Append @p text to the NUL-terminated @p buffer, truncating at @p capacity. */
static void append_text(char *buffer, size_t capacity, const char *text)
{
    size_t length = strnlen(buffer, capacity - 1);
    while (*text && length < capacity - 1) {
        buffer[length++] = *text++;
    }
    buffer[length] = '\0';
}
