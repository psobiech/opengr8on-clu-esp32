// Host tests for the crash record (main/clu_crash.c, included): what the panic wrapper and the
// task watchdog hook store, and what the next boot reports. A "reboot" keeps the RTC record and
// clears everything else, as the chip does after a panic.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../main/clu_crash.c"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- fake ESP-IDF
bool g_panic_abort;
char *g_panic_abort_details;
static int s_real_handler_calls;
void __real_esp_panic_handler(panic_info_t *info) { (void)info; s_real_handler_calls++; }

static esp_reset_reason_t s_reset_reason;
esp_reset_reason_t esp_reset_reason(void) { return s_reset_reason; }

static int s_task_handle_storage;
static const char *s_current_task = "clu_lua"; // NULL: the scheduler has not started
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return s_current_task ? &s_task_handle_storage : NULL; }
char *pcTaskGetName(TaskHandle_t task) { (void)task; return (char *)s_current_task; }

static const char *s_stuck_tasks[4];
esp_err_t esp_task_wdt_print_triggered_tasks(task_wdt_msg_handler msg_handler, void *opaque, int *cpus_fail)
{
    (void)cpus_fail;
    msg_handler(opaque, "Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:");
    for (int i = 0; i < 4 && s_stuck_tasks[i]; i++) {
        msg_handler(opaque, "\n - ");
        msg_handler(opaque, s_stuck_tasks[i]);
        msg_handler(opaque, " (CPU 1)");
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- helpers
/** @brief Panic with @p reason at @p program_counter (an exception: no abort, no watchdog). */
static void panic_at(const char *reason, uintptr_t program_counter)
{
    panic_info_t info = {.core = 1, .exception = PANIC_EXCEPTION_FAULT, .reason = reason, .addr = (const void *)program_counter};
    __wrap_esp_panic_handler(&info);
}

/** @brief Restart the chip with @p reset_reason: RAM cleared, the RTC record kept, then boot. */
static const char *reboot(esp_reset_reason_t reset_reason)
{
    memset(s_watchdog_tasks, 0, sizeof s_watchdog_tasks);
    memset(s_last_crash, 0, sizeof s_last_crash);
    g_panic_abort = false;
    g_panic_abort_details = NULL;
    s_reset_reason = reset_reason;
    clu_crash_init();
    const char *last_crash = clu_crash_last();
    return last_crash ? last_crash : "(none)";
}

static void check_str(const char *name, const char *got, const char *want)
{
    int equal = strcmp(got, want) == 0;
    check(name, equal);
    if (!equal) {
        printf("     got  '%s'\n     want '%s'\n", got, want);
    }
}

int main(void)
{
    memset(&s_record, 0xa5, sizeof s_record); // power-on: RTC memory holds garbage
    check_str("power-on with garbage in RTC memory: no crash", reboot(ESP_RST_POWERON), "(none)");

    panic_at("LoadProhibited", 0x400d1234);
    check("exception: ESP-IDF's handler still runs", s_real_handler_calls == 1);
    check_str("exception: cause, task and PC after the panic reset", reboot(ESP_RST_PANIC),
              "LoadProhibited | task clu_lua | pc 0x400d1234");
    check_str("record consumed: the next reboot reports nothing", reboot(ESP_RST_SW), "(none)");
    panic_at("LoadProhibited", 0x400d1234);
    check_str("record ignored after a reset that is not a crash (power cut)", reboot(ESP_RST_POWERON), "(none)");

    g_panic_abort = true;
    g_panic_abort_details = "assert failed: tfbus_master.c:120 (module)";
    s_current_task = "tfbus";
    panic_at(NULL, 0x400e0000);
    check_str("assert: the abort details", reboot(ESP_RST_PANIC),
              "assert failed: tfbus_master.c:120 (module) | task tfbus | pc 0x400e0000");
    g_panic_abort = true;
    panic_at(NULL, 0x400e0004);
    check_str("abort() without details", reboot(ESP_RST_PANIC), "abort() | task tfbus | pc 0x400e0004");

    s_stuck_tasks[0] = "clu_lua";
    s_stuck_tasks[1] = "tfbus";
    s_current_task = "IDLE1";
    esp_task_wdt_isr_user_handler();
    g_panic_abort = true; // the watchdog panics through abort, without details
    panic_at(NULL, 0x40081000);
    check_str("task watchdog: the tasks that stopped checking in", reboot(ESP_RST_TASK_WDT),
              "task watchdog: clu_lua (CPU 1), tfbus (CPU 1) | task IDLE1 | pc 0x40081000");
    s_stuck_tasks[0] = s_stuck_tasks[1] = NULL;

    panic_at("Interrupt wdt timeout on CPU0", 0x40082000);
    check_str("interrupt watchdog", reboot(ESP_RST_INT_WDT), "Interrupt wdt timeout on CPU0 | task IDLE1 | pc 0x40082000");

    s_current_task = NULL;
    panic_at("IllegalInstruction", 0x40080000);
    check_str("panic before the scheduler runs: no task", reboot(ESP_RST_PANIC), "IllegalInstruction | task - | pc 0x40080000");
    s_current_task = "clu_lua";

    char long_reason[200];
    memset(long_reason, 'x', sizeof long_reason - 1);
    long_reason[sizeof long_reason - 1] = '\0';
    panic_at(long_reason, 0x1);
    check("long cause truncated to the record", strlen(reboot(ESP_RST_PANIC)) < sizeof s_last_crash &&
                                                    strlen(s_record.cause) == sizeof s_record.cause - 1);

    memset(&s_record, 'A', sizeof s_record); // a record without terminators (ASan checks the reads)
    s_record.magic = RECORD_MAGIC;
    check("unterminated record: read within bounds", strlen(reboot(ESP_RST_PANIC)) < sizeof s_last_crash);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
