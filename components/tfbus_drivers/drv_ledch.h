/**
 * @file drv_ledch.h
 * @brief LED_RGBW_DIN channel objects: LED_CHANNEL (class 49, x4) and LED_CCT (class 72, x2).
 *
 * Part of the LED_RGBW_DIN driver (drv_ledrgb.c), which owns the module state and forwards the
 * hooks for these classes here. See drv_ledch.c for the wire format.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lua.h"
#include "tfbus_master.h"

enum { LEDCH_CLS_CHANNEL = 49, LEDCH_CLS_CCT = 72 };

typedef struct ledch ledch_t;

/** @brief Channel object state (allocated, never NULL). */
ledch_t *ledch_new(void);

/** @brief Remember the name of a LED_CHANNEL / LED_CCT object. */
void ledch_io_new(ledch_t *ledch_state, int cls, int channel_index, const char *name);

/** @brief tfbus_driver_t::io_set for classes 49 / 72; false = store generically. */
bool ledch_io_set(tfbus_module_t *module, ledch_t *ledch_state, lua_State *L, int cls, int channel_index, int idx, int val);

/** @brief tfbus_driver_t::io_execute for classes 49 / 72; -1 = unknown method. */
int ledch_io_execute(tfbus_module_t *module, ledch_t *ledch_state, lua_State *L, int obj, int cls, int channel_index, int idx,
                     int first, int nargs);

/** @brief A channel value reported by the module (status record 04 02 08 [ch, value]). */
void ledch_reported(ledch_t *ledch_state, int channel_index, uint8_t reported_value);

/** @brief End of a status: LED_CCT updates from the channel pairs reported in it. */
void ledch_reported_end(ledch_t *ledch_state);

/** @brief Re-apply what Lua set (module re-addressed / back online). */
void ledch_init(tfbus_module_t *module, ledch_t *ledch_state);

/** @brief Stop the timers. */
void ledch_deinit(ledch_t *ledch_state);

/** @brief tfbus_driver_t::overdue helper: run the expired channel timers. */
void ledch_overdue(ledch_t *ledch_state);
