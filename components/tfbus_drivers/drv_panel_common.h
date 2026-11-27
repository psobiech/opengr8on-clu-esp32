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
    int32_t threshold, min, max;      /**< Event threshold and valid range [1/100]. */
    uint16_t sensitivity_ms;          /**< Sensitivity: applied by the panel, sent with the threshold. */
    int32_t value, last_event_value;  /**< Current value and value at the last change event [1/100]. */
    bool have, out_of_range;          /**< Value received; currently out of [min, max]. */
} panel_sensor_t;

/**
 * @brief Update a sensor value and fire its events.
 *
 * Events: 0 OnValueChange, 1 OnValueRise, 2 OnValueLower, 3 OnOutOfRange.
 *
 * @param sensor Sensor.
 * @param new_value New value [1/100] (posted to Lua as new_value / 100).
 */
void panel_sensor_update(panel_sensor_t *sensor, int32_t new_value);

/**
 * @brief Bind a sensor to its Lua object (io_new) and post the value received before that.
 *
 * The first state frame after addressing arrives before OM.LUA creates the objects; a panel that
 * reports temperature only on change would otherwise leave Value 0 until the next change.
 *
 * @param sensor Sensor.
 * @param name   Lua object name.
 */
void panel_sensor_name(panel_sensor_t *sensor, const char *name);
/**
 * @brief Apply a panel button event record (TOUCH_PANEL_FM_4: TLV 0x08, VAL = button*7 + event).
 *
 * OnSwitchOn / OnSwitchOff also update Value.
 *
 * @param btn_names   Button object names (empty = not declared).
 * @param button_count Number of buttons.
 * @param values      Button state bitmask.
 * @param event_value Panel event value: button*7 + event.
 */
void panel_button_event(char (*btn_names)[24], int button_count, uint32_t *values, int event_value);

/**
 * @brief Sensor config the panel applies: [sensor_id, Sensitivity / 100 ms (max 255), Threshold x 10 LE16].
 *
 * Checked on a real CLU (SMART_PANEL class frame 0x16, light: Threshold 0.3 -> 03 00, 30 -> 2c 01;
 * Sensitivity 1234 ms -> 0c, 30000 ms -> ff). MinValue / MaxValue are not sent.
 *
 * @param sensor         Sensor (threshold).
 * @param sensor_id      Panel's sensor id.
 * @param sensitivity_ms Sensitivity to send (the stored one, or a value replayed from a capture).
 * @param out            4 bytes.
 */
void panel_sensor_config(const panel_sensor_t *sensor, uint8_t sensor_id, uint16_t sensitivity_ms, uint8_t out[4]);
