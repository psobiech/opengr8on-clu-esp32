/**
 * @file clu_i2c.h
 * @brief Shared I2C master buses: one per SDA/SCL pin pair, so several drivers (RTC, on-board IO
 * expander) can sit on the same wires.
 */
#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The I2C master bus on these pins, created on first use (any free controller, internal
 * pull-ups on).
 *
 * Not thread-safe: call while the drivers are set up (boot). The returned bus itself is safe to
 * use from several tasks (the IDF driver locks it per transfer).
 *
 * @return Bus handle, NULL if a pin is < 0, a pin is already used by another pair, or the bus
 *         could not be created.
 */
i2c_master_bus_handle_t clu_i2c_bus(int sda_gpio, int scl_gpio);

#ifdef __cplusplus
}
#endif
