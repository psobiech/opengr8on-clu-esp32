/**
 * @file clu_crash.h
 * @brief What crashed the previous boot: a short record kept in RTC memory across the reset.
 *
 * The panic handler (linked with -Wl,--wrap=esp_panic_handler, see CMakeLists.txt) stores the
 * cause, the task and the program counter; a task watchdog timeout adds the tasks that stopped
 * checking in. No flash is written, so nothing survives a power cut.
 */
#pragma once

#include <stdbool.h>

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

/**
 * @brief True when the last boots in a row each crashed early (within 2 minutes) in the Lua task:
 *        it panicked, stopped checking in with the task watchdog, or starved its core (IDLE1).
 *
 * MAIN.LUA then must not run again (clu_lua_cfg_t::skip_main_script): a script that hangs or
 * busy-loops would otherwise reboot the CLU forever. Valid after clu_crash_init().
 */
bool clu_crash_lua_boot_loop(void);
