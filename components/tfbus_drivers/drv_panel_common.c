/**
 * @file drv_panel_common.c
 * @brief Panel sensor hysteresis events and button event handling (monostable / bistable / locked).
 */
#include "drv_panel_common.h"

#include <math.h>

#include "clu_lua.h"

/** @brief Sensor event indices (OnValueChange, OnValueRise, OnValueLower, OnOutOfRange). */
enum { SEV_CHANGE = 0, SEV_RISE = 1, SEV_LOWER = 2, SEV_RANGE = 3 };
/** @brief Button event indices (OnChange, OnSwitchOn, OnSwitchOff). */
enum { EV_CHANGE = 0, EV_ON = 1, EV_OFF = 2 };

void panel_sensor_update(panel_sensor_t *s, double v)
{
    if (!s->name[0]) {
        return;
    }
    if (!s->have) {
        s->have = true;
        s->value = s->last_event_value = v;
        s->out_of_range = v < s->min || v > s->max;
        clu_lua_post_number(s->name, 0, v);
        return;
    }
    if (v == s->value) {
        return;
    }
    s->value = v;
    clu_lua_post_number(s->name, 0, v);
    double d = v - s->last_event_value;
    if (fabs(d) >= s->threshold - 1e-9) {
        s->last_event_value = v;
        clu_lua_post_event(s->name, SEV_CHANGE);
        clu_lua_post_event(s->name, d > 0 ? SEV_RISE : SEV_LOWER);
    }
    bool oor = v < s->min || v > s->max;
    if (oor && !s->out_of_range) {
        clu_lua_post_event(s->name, SEV_RANGE);
    }
    s->out_of_range = oor;
}

void panel_button_event(char (*btn_names)[24], int n, uint32_t *values, const uint8_t *modes,
                        int val)
{
    int b = val / 7, ev = val % 7;
    if (b < 0 || b >= n || !btn_names[b][0]) {
        return;
    }
    const char *name = btn_names[b];
    int mode = modes ? modes[b] : PANEL_MODE_MONOSTABLE;
    if (mode == PANEL_MODE_LOCKED) {
        return;
    }
    if (mode == PANEL_MODE_BISTABLE) {
        // The panel reports press/release (monostable); the CLU turns each press into a toggle.
        // Release and the panel's OnChange events carry no state change here.
        if (ev == EV_ON) {
            bool on = !(*values & (1u << b));
            *values = on ? (*values | (1u << b)) : (*values & ~(1u << b));
            clu_lua_post_number(name, 0, on);
            clu_lua_post_event(name, on ? EV_ON : EV_OFF);
            clu_lua_post_event(name, EV_CHANGE);
        } else if (ev != EV_OFF && ev != EV_CHANGE) {
            clu_lua_post_event(name, ev); // OnClick / OnShortPress / OnLongPress / OnHold
        }
        return;
    }
    if (ev == EV_ON || ev == EV_OFF) {
        bool on = ev == EV_ON;
        *values = on ? (*values | (1u << b)) : (*values & ~(1u << b));
        clu_lua_post_number(name, 0, on);
    }
    clu_lua_post_event(name, ev);
}
