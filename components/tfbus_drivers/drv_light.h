/**
 * @file drv_light.h
 * @brief Shared logic for dimmable light modules (DIMMER_MOSFET_*, LED_RGBW_*).
 *
 * Timed switching (Switch/SwitchOn/SwitchOff with Time > 0), HoldValue stepping, method argument
 * helpers and the SET_RAMP_TIME record.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_timer.h"
#include "lua.h"
#include "tfbus_master.h"

/** @brief Max ramp time sent to a module [ms] (interface range 0-15000; wire field is u16). */
#define LIGHT_RAMP_MAX 15000

/** @brief Timer callback, run in the bus task under the driver lock. */
typedef void (*light_timer_fn_t)(tfbus_module_t *m, void *owner);

/**
 * @brief One-shot timer whose expiry runs in the bus task (via tfbus_master_defer).
 *
 * A fire already queued when the timer is restarted / cancelled is dropped (generation check).
 */
typedef struct {
    tfbus_module_t *m;
    void *owner;
    light_timer_fn_t fn;
    volatile uint32_t gen;
    esp_timer_handle_t timer;
} light_timer_t;

/**
 * @brief Start (or restart) the timer.
 * @param t     Timer (zero-initialised before first use).
 * @param m     Module.
 * @param owner Passed to @p fn.
 * @param fn    Expiry callback.
 * @param ms    Delay [ms].
 */
void light_timer_start(light_timer_t *t, tfbus_module_t *m, void *owner, light_timer_fn_t fn,
                       uint32_t ms);

/** @brief Cancel the timer (a fire already queued becomes stale). */
void light_timer_cancel(light_timer_t *t);

/** @brief Stop and delete the timer (driver deinit). */
void light_timer_delete(light_timer_t *t);

/**
 * @brief HoldValue state: repeated calls (from a button's OnHold) step the value.
 *
 * A call more than @ref LIGHT_HOLD_GAP_MS after the previous one starts a new hold, which
 * reverses the direction (or moves away from the end the value sits at).
 */
typedef struct {
    int64_t last_us;  /**< Time of the previous call (0 = none). */
    int8_t dir;       /**< +1 up, -1 down, 0 = never held. */
} light_hold_t;

/** @brief Gap between HoldValue calls that starts a new hold [ms]. */
#define LIGHT_HOLD_GAP_MS 550

/** @brief Step time of the first call of a hold [ms] (default DIN HoldInterval). */
#define LIGHT_HOLD_FIRST_MS 100

/**
 * @brief Advance a hold.
 * @param h      Hold state.
 * @param now_us Current time [us].
 * @param cur    Current value.
 * @param lo     Lower end.
 * @param hi     Upper end.
 * @return Time this step covers [ms]; h->dir holds the direction.
 */
uint32_t light_hold_step(light_hold_t *h, int64_t now_us, double cur, double lo, double hi);

/**
 * @brief Optional method argument @p i (0-based) as a number.
 * @return The argument, or @p dflt if absent / nil.
 */
double light_arg(lua_State *L, int first, int nargs, int i, double dflt);

/** @brief Ramp argument @p i [ms] clamped to 0..LIGHT_RAMP_MAX; absent -> @p dflt. */
uint32_t light_ramp_arg(lua_State *L, int first, int nargs, int i, uint32_t dflt);

/** @brief Queue SET_RAMP_TIME (record 0x06, data [0, ms LE u16, 0, 0]). */
void light_queue_ramp(tfbus_module_t *m, uint32_t ms);

/** @brief Clamp @p v to [lo, hi]. */
static inline double light_clamp(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
