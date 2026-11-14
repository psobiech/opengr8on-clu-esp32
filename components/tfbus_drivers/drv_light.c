/**
 * @file drv_light.c
 * @brief Shared logic for dimmable light modules: timers, Hold* direction, SET_RAMP_TIME.
 */
#include "drv_light.h"

#include "lauxlib.h"
#include "tfbus_drivers.h"

/** @brief Deferred expiry (bus task): run the callback unless restarted / cancelled since. */
static void timer_fire(tfbus_module_t *module, void *arg, uint32_t gen)
{
    light_timer_t *timer_state = arg;
    if (gen == timer_state->gen && timer_state->due_us) {
        timer_state->due_us = 0;
        timer_state->fn(module, timer_state->owner);
    }
}

/** @brief esp_timer callback: no work here, hand it to the bus task. */
static void timer_cb(void *arg)
{
    light_timer_t *timer_state = arg;
    tfbus_master_defer(timer_fire, timer_state->m, timer_state, timer_state->gen);
}

void light_timer_start(light_timer_t *timer_state, tfbus_module_t *module, void *owner, light_timer_fn_t expiry_callback,
                       uint32_t ms)
{
    if (!timer_state->timer) {
        const esp_timer_create_args_t timer_args = {.callback = timer_cb, .arg = timer_state, .name = "light"};
        if (esp_timer_create(&timer_args, &timer_state->timer) != ESP_OK) {
            return;
        }
    }
    timer_state->m = module;
    timer_state->owner = owner;
    timer_state->fn = expiry_callback;
    esp_timer_stop(timer_state->timer);
    timer_state->gen++;
    timer_state->due_us = esp_timer_get_time() + (int64_t)ms * 1000;
    esp_timer_start_once(timer_state->timer, (uint64_t)ms * 1000);
}

void light_timer_cancel(light_timer_t *timer_state)
{
    timer_state->gen++;
    timer_state->due_us = 0;
    if (timer_state->timer) {
        esp_timer_stop(timer_state->timer);
    }
}

void light_timer_delete(light_timer_t *timer_state)
{
    if (timer_state->timer) {
        esp_timer_stop(timer_state->timer);
        esp_timer_delete(timer_state->timer);
        timer_state->timer = NULL;
    }
    timer_state->gen++;
    timer_state->due_us = 0;
}

void light_timer_overdue(light_timer_t *timer_state)
{
    if (timer_state->due_us && esp_timer_get_time() >= timer_state->due_us) {
        timer_state->due_us = 0; // a deferred call still queued finds it done
        timer_state->fn(timer_state->m, timer_state->owner);
    }
}

int light_hold_dir(light_hold_t *hold_state, int64_t now_us, double cur, double lower_bound, double upper_bound)
{
    int64_t gap_ms = (now_us - hold_state->last_us) / 1000;
    bool fresh = hold_state->last_us == 0 || gap_ms > LIGHT_HOLD_GAP_MS || gap_ms < 0;
    hold_state->last_us = now_us;
    if (!fresh) {
        return hold_state->dir; // within a hold the value stays at an end it reaches
    }
    hold_state->dir = hold_state->dir > 0 ? -1 : 1; // first hold: up
    if (cur >= upper_bound) {
        hold_state->dir = -1;
    } else if (cur <= lower_bound) {
        hold_state->dir = 1;
    }
    return hold_state->dir;
}

double light_arg(lua_State *L, int first, int nargs, int argument_index, double dflt)
{
    if (argument_index >= nargs || lua_isnoneornil(L, first + argument_index)) {
        return dflt;
    }
    return luaL_checknumber(L, first + argument_index);
}

uint32_t light_ramp_arg(lua_State *L, int first, int nargs, int argument_index, uint32_t dflt)
{
    double ramp_value = light_arg(L, first, nargs, argument_index, dflt);
    return (uint32_t)light_clamp(ramp_value, 0, LIGHT_RAMP_MAX);
}

void light_queue_ramp(tfbus_module_t *module, uint32_t ms)
{
    if (ms > LIGHT_RAMP_MAX) {
        ms = LIGHT_RAMP_MAX;
    }
    drv_queue_record(module, 0x06, 0x00, (uint8_t[]){0, (uint8_t)ms, (uint8_t)(ms >> 8), 0, 0}, 5);
}

void light_queue_dlg(tfbus_module_t *module, uint8_t channel, uint32_t group)
{
    if (group > 10000) {
        group = 10000;
    }
    drv_queue_record(module, 0x0f, 0x00, (uint8_t[]){channel, 1, (uint8_t)group, (uint8_t)(group >> 8)}, 4);
}
