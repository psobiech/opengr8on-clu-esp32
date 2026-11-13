/**
 * @file drv_light.c
 * @brief Shared logic for dimmable light modules: timers, Hold* direction, SET_RAMP_TIME.
 */
#include "drv_light.h"

#include "lauxlib.h"
#include "tfbus_drivers.h"

/** @brief Deferred expiry (bus task): run the callback unless restarted / cancelled since. */
static void timer_fire(tfbus_module_t *m, void *arg, uint32_t gen)
{
    light_timer_t *t = arg;
    if (gen == t->gen && t->due_us) {
        t->due_us = 0;
        t->fn(m, t->owner);
    }
}

/** @brief esp_timer callback: no work here, hand it to the bus task. */
static void timer_cb(void *arg)
{
    light_timer_t *t = arg;
    tfbus_master_defer(timer_fire, t->m, t, t->gen);
}

void light_timer_start(light_timer_t *t, tfbus_module_t *m, void *owner, light_timer_fn_t fn,
                       uint32_t ms)
{
    if (!t->timer) {
        const esp_timer_create_args_t a = {.callback = timer_cb, .arg = t, .name = "light"};
        if (esp_timer_create(&a, &t->timer) != ESP_OK) {
            return;
        }
    }
    t->m = m;
    t->owner = owner;
    t->fn = fn;
    esp_timer_stop(t->timer);
    t->gen++;
    t->due_us = esp_timer_get_time() + (int64_t)ms * 1000;
    esp_timer_start_once(t->timer, (uint64_t)ms * 1000);
}

void light_timer_cancel(light_timer_t *t)
{
    t->gen++;
    t->due_us = 0;
    if (t->timer) {
        esp_timer_stop(t->timer);
    }
}

void light_timer_delete(light_timer_t *t)
{
    if (t->timer) {
        esp_timer_stop(t->timer);
        esp_timer_delete(t->timer);
        t->timer = NULL;
    }
    t->gen++;
    t->due_us = 0;
}

void light_timer_overdue(light_timer_t *t)
{
    if (t->due_us && esp_timer_get_time() >= t->due_us) {
        t->due_us = 0; // a deferred call still queued finds it done
        t->fn(t->m, t->owner);
    }
}

int light_hold_dir(light_hold_t *h, int64_t now_us, double cur, double lo, double hi)
{
    int64_t gap_ms = (now_us - h->last_us) / 1000;
    bool fresh = h->last_us == 0 || gap_ms > LIGHT_HOLD_GAP_MS || gap_ms < 0;
    h->last_us = now_us;
    if (!fresh) {
        return h->dir; // within a hold the value stays at an end it reaches
    }
    h->dir = h->dir > 0 ? -1 : 1; // first hold: up
    if (cur >= hi) {
        h->dir = -1;
    } else if (cur <= lo) {
        h->dir = 1;
    }
    return h->dir;
}

double light_arg(lua_State *L, int first, int nargs, int i, double dflt)
{
    if (i >= nargs || lua_isnoneornil(L, first + i)) {
        return dflt;
    }
    return luaL_checknumber(L, first + i);
}

uint32_t light_ramp_arg(lua_State *L, int first, int nargs, int i, uint32_t dflt)
{
    double v = light_arg(L, first, nargs, i, dflt);
    return (uint32_t)light_clamp(v, 0, LIGHT_RAMP_MAX);
}

void light_queue_ramp(tfbus_module_t *m, uint32_t ms)
{
    if (ms > LIGHT_RAMP_MAX) {
        ms = LIGHT_RAMP_MAX;
    }
    drv_queue_record(m, 0x06, 0x00, (uint8_t[]){0, (uint8_t)ms, (uint8_t)(ms >> 8), 0, 0}, 5);
}

void light_queue_dlg(tfbus_module_t *m, uint8_t ch, uint32_t group)
{
    if (group > 10000) {
        group = 10000;
    }
    drv_queue_record(m, 0x0f, 0x00, (uint8_t[]){ch, 1, (uint8_t)group, (uint8_t)(group >> 8)}, 4);
}
