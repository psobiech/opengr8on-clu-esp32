/**
 * @file drv_psu.h
 * @brief PowerSupplyVoltage (class 12) of TF-Bus modules: Value / Value% and events from the
 *        supply voltage status record.
 *
 * Status record: 02 02 10 [U LE u16, 0.01 V]. Same features on every module that has it:
 * 0 Value, 1 Value%, 3 Sensitivity (0.5 V), 5 MinValue (21 V), 6 MaxValue (24 V).
 * Events: 0 OnValueChange, 1 OnValueLower, 2 OnValueRise, 3 OnOutOfRange, 6 OnInRange.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lua.h"

/** @brief OM class of PowerSupplyVoltage. */
#define PSU_CLASS 12
/** @brief Status record type carrying the supply voltage. */
#define PSU_STATUS_RECORD 0x02

/** @brief One PowerSupplyVoltage object. */
typedef struct {
    char name[24];                          /**< Lua object name (empty = not declared). */
    int32_t voltage, last_event_voltage;    /**< Last value, value at the last change event [10 mV]. */
    int32_t sensitivity, min, max;          /**< Change threshold and valid range [10 mV]. */
    bool have_voltage, out_of_range;        /**< A value was received; currently out of [min, max]. */
} psu_t;

/**
 * @brief Set the interface defaults (Sensitivity 0.5 V, MinValue 21 V, MaxValue 24 V).
 * @param psu PowerSupplyVoltage.
 */
void psu_init(psu_t *psu);

/**
 * @brief tfbus_driver_t::io_set part: cache Sensitivity / MinValue / MaxValue.
 * @param psu PowerSupplyVoltage.
 * @param L Lua state.
 * @param idx Feature index.
 * @param val Stack index of the value.
 * @return false: the value is also stored generically.
 */
bool psu_io_set(psu_t *psu, lua_State *L, int idx, int val);

/**
 * @brief New supply voltage from the status record: Value / Value% and events.
 * @param psu PowerSupplyVoltage.
 * @param voltage Supply voltage [10 mV].
 */
void psu_update(psu_t *psu, int32_t voltage);
