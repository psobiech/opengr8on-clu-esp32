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

void panel_sensor_update(panel_sensor_t *sensor, double new_value)
{
    if (!sensor->name[0]) {
        return;
    }
    if (!sensor->have) {
        sensor->have = true;
        sensor->value = sensor->last_event_value = new_value;
        sensor->out_of_range = new_value < sensor->min || new_value > sensor->max;
        clu_lua_post_number(sensor->name, 0, new_value);
        return;
    }
    if (new_value == sensor->value) {
        return;
    }
    sensor->value = new_value;
    clu_lua_post_number(sensor->name, 0, new_value);
    double value_delta = new_value - sensor->last_event_value;
    if (fabs(value_delta) >= sensor->threshold - 1e-9) {
        sensor->last_event_value = new_value;
        clu_lua_post_event(sensor->name, SEV_CHANGE);
        clu_lua_post_event(sensor->name, value_delta > 0 ? SEV_RISE : SEV_LOWER);
    }
    bool oor = new_value < sensor->min || new_value > sensor->max;
    if (oor && !sensor->out_of_range) {
        clu_lua_post_event(sensor->name, SEV_RANGE);
    }
    sensor->out_of_range = oor;
}

void panel_button_event(char (*btn_names)[24], int button_count, uint32_t *values, const uint8_t *modes,
                        int event_value)
{
    int button_index = event_value / 7, event_index = event_value % 7;
    if (button_index < 0 || button_index >= button_count || !btn_names[button_index][0]) {
        return;
    }
    const char *name = btn_names[button_index];
    int mode = modes ? modes[button_index] : PANEL_MODE_MONOSTABLE;
    if (mode == PANEL_MODE_LOCKED) {
        return;
    }
    if (mode == PANEL_MODE_BISTABLE) {
        // The panel reports press/release (monostable); the CLU turns each press into a toggle.
        // Release and the panel's OnChange events carry no state change here.
        if (event_index == EV_ON) {
            bool on = !(*values & (1u << button_index));
            *values = on ? (*values | (1u << button_index)) : (*values & ~(1u << button_index));
            clu_lua_post_number(name, 0, on);
            clu_lua_post_event(name, on ? EV_ON : EV_OFF);
            clu_lua_post_event(name, EV_CHANGE);
        } else if (event_index != EV_OFF && event_index != EV_CHANGE) {
            clu_lua_post_event(name, event_index); // OnClick / OnShortPress / OnLongPress / OnHold
        }
        return;
    }
    if (event_index == EV_ON || event_index == EV_OFF) {
        bool on = event_index == EV_ON;
        *values = on ? (*values | (1u << button_index)) : (*values & ~(1u << button_index));
        clu_lua_post_number(name, 0, on);
    }
    clu_lua_post_event(name, event_index);
}
