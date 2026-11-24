/**
 * @file clu_crash.h
 * @brief What crashed the previous boot: a short record kept in RTC memory across the reset.
 *
 * The panic handler (linked with -Wl,--wrap=esp_panic_handler, see CMakeLists.txt) stores the
 * cause, the task and the program counter; a task watchdog timeout adds the tasks that stopped
 * checking in. No flash is written, so nothing survives a power cut.
 */
#pragma once

/**
 * @brief Take over the record of the previous boot (call once, early in app_main).
 *
 * Kept and logged only when this boot follows a panic or watchdog reset; the record is cleared
 * for the next crash either way.
 */
void clu_crash_init(void);

/**
 * @brief The crash that caused this boot, e.g.
 *        "task watchdog: clu_lua (CPU 1) | task IDLE1 | pc 0x400d1234".
 * @return NULL when this boot did not follow a recorded crash.
 */
const char *clu_crash_last(void);
