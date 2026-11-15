/**
 * @file tfbus_drivers.h
 * @brief TFBUS module drivers (per hwType).
 *
 * Register all with tfbus_drivers_register_all() before tfbus_master_init().
 */
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include "tfbus_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register every built-in driver with the bus master. */
void tfbus_drivers_register_all(void);

/**
 * @brief Queue a single-record MODULE_CMD to the module.
 * @param module Module.
 * @param type Record type.
 * @param meta Record meta byte.
 * @param data Record data.
 * @param len  Data length.
 */
void drv_queue_record(tfbus_module_t *module, uint8_t type, uint8_t meta, const uint8_t *data, uint8_t len);

/**
 * @brief Queue a multi-record MODULE_CMD (batched records, as the real CLU does for CHANNEL_MAP pairs).
 * @param module Module.
 * @param recs Records.
 * @param record_count Number of records.
 */
void drv_queue_records(tfbus_module_t *module, const tfbus_tlv_t *recs, size_t record_count);

/**
 * @brief Queue a raw CLU->module frame of the given type (class frames 0x10-0x30 etc.).
 * @param module  Module.
 * @param type    Frame type.
 * @param payload Payload.
 * @param len     Payload length.
 */
void drv_queue_frame(tfbus_module_t *module, uint8_t type, const uint8_t *payload, size_t len);

/** @name Built-in drivers
 * @{ */
extern const tfbus_driver_t tfbus_drv_io_module_din8;
extern const tfbus_driver_t tfbus_drv_smart_panel_fm4;
extern const tfbus_driver_t tfbus_drv_roller_sh_din3;
extern const tfbus_driver_t tfbus_drv_touch_panel_fm4;
extern const tfbus_driver_t tfbus_drv_relay_din4;
extern const tfbus_driver_t tfbus_drv_roller_sh_din;
extern const tfbus_driver_t tfbus_drv_dimmer_mosfet_din;
extern const tfbus_driver_t tfbus_drv_led_rgbw_din;
/** @} */

/**
 * @brief Lua number -> hundredths (21.5 -> 2150), rounded; NaN -> 0, saturated at +-2e9.
 *
 * Drivers keep sensor values / limits as integer hundredths: exact comparisons, no FPU in the bus task.
 */
static inline int32_t drv_centi(float value)
{
    return !(value == value) ? 0 : value >= 2e7f ? 2000000000 : value <= -2e7f ? -2000000000 : (int32_t)lroundf(value * 100.0f);
}

#ifdef __cplusplus
}
#endif
