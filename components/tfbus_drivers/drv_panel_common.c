/**
 * @file drv_panel_common.c
 * @brief Panel sensor hysteresis events and touch panel button event records.
 */
#include "drv_panel_common.h"

#include <stdlib.h>

#include "clu_lua.h"

/** @brief Sensor event indices (OnValueChange, OnValueRise, OnValueLower, OnOutOfRange). */
enum { SEV_CHANGE = 0, SEV_RISE = 1, SEV_LOWER = 2, SEV_RANGE = 3 };
/** @brief Button event indices (OnChange, OnSwitchOn, OnSwitchOff). */
enum { EV_CHANGE = 0, EV_ON = 1, EV_OFF = 2 };

void panel_sensor_update(panel_sensor_t *sensor, int32_t new_value)
{
    if (!sensor->name[0]) {
        return;
    }
    if (!sensor->have) {
        sensor->have = true;
        sensor->value = sensor->last_event_value = new_value;
        sensor->out_of_range = new_value < sensor->min || new_value > sensor->max;
        clu_lua_post_number(sensor->name, 0, (float)new_value / 100.0f);
        return;
    }
    if (new_value == sensor->value) {
        return;
    }
    sensor->value = new_value;
    clu_lua_post_number(sensor->name, 0, (float)new_value / 100.0f);
    int32_t value_delta = new_value - sensor->last_event_value;
    if (abs(value_delta) >= sensor->threshold) {
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

void panel_button_event(char (*btn_names)[24], int button_count, uint32_t *values, int event_value)
{
    int button_index = event_value / 7, event_index = event_value % 7;
    if (button_index < 0 || button_index >= button_count || !btn_names[button_index][0]) {
        return;
    }
    const char *name = btn_names[button_index];
    if (event_index == EV_ON || event_index == EV_OFF) {
        bool on = event_index == EV_ON;
        *values = on ? (*values | (1u << button_index)) : (*values & ~(1u << button_index));
        clu_lua_post_number(name, 0, on);
    }
    clu_lua_post_event(name, event_index);
}

void panel_sensor_config(const panel_sensor_t *sensor, uint8_t sensor_id, uint16_t sensitivity_ms, uint8_t out[4])
{
    unsigned sensitivity_units = sensitivity_ms / 100u > 255u ? 255u : sensitivity_ms / 100u;
    int32_t threshold_deci = sensor->threshold / 10; // 1/100 -> 1/10
    uint16_t threshold_wire = threshold_deci < 0 ? 0 : threshold_deci > 0xffff ? 0xffff : (uint16_t)threshold_deci;
    out[0] = sensor_id;
    out[1] = (uint8_t)sensitivity_units;
    out[2] = (uint8_t)threshold_wire;
    out[3] = (uint8_t)(threshold_wire >> 8);
}
