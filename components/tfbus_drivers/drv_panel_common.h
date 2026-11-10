/**
 * @file drv_panel_common.h
 * @brief Shared logic for panel modules (TOUCH_PANEL_*, SMART_PANEL_*).
 *
 * Sensor hysteresis events and panel-generated button events (VAL = btn*7 + event).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** @brief Panel sensor (temperature, light, ...) with change-threshold events. */
typedef struct {
    char name[24];                    /**< Lua object name (empty = not declared). */
    double threshold, min, max;       /**< Event threshold and valid range. */
    double value, last_event_value;   /**< Current value and value at the last change event. */
    bool have, out_of_range;          /**< Value received; currently out of [min, max]. */
} panel_sensor_t;

/**
 * @brief Update a sensor value and fire its events.
 *
 * Events: 0 OnValueChange, 1 OnValueRise, 2 OnValueLower, 3 OnOutOfRange.
 *
 * @param s Sensor.
 * @param v New value.
 */
void panel_sensor_update(panel_sensor_t *s, double v);

/** @brief PANEL_BUTTON feature 1 Mode (SMART_PANEL_FM_4 interface). */
enum { PANEL_MODE_MONOSTABLE = 0, PANEL_MODE_BISTABLE = 1, PANEL_MODE_LOCKED = 2 };

/**
 * @brief Handle a panel button event.
 *
 * Bistable is implemented here, in the CLU: each press toggles Value and fires
 * OnSwitchOn/OnSwitchOff + OnChange; Locked drops all events.
 *
 * @param btn_names Array of n names (empty = not declared).
 * @param n         Number of buttons.
 * @param values    Button state bitmask.
 * @param modes     Per-button Mode (NULL = all monostable).
 * @param val       Panel event value: btn*7 + event.
 */
void panel_button_event(char (*btn_names)[24], int n, uint32_t *values, const uint8_t *modes,
                        int val);
