/**
 * @file pcf85063.h
 * @brief PCF85063A/TP real-time clock: register map and time register encoding (no I/O).
 *
 * The seven time registers (Seconds .. Years, 0x04..0x0A) hold UTC in BCD, 24-hour mode,
 * years 2000..2099. Bit 7 of Seconds (OS) is set by the chip when its oscillator stopped (power
 * lost with no backup cell): the stored time is then invalid until written again.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define PCF85063_I2C_ADDR     0x51
#define PCF85063_REG_CONTROL1 0x00
#define PCF85063_REG_SECONDS  0x04
#define PCF85063_TIME_LEN     7 /**< Seconds, Minutes, Hours, Days, Weekdays, Months, Years */

#define PCF85063_CTRL1_STOP  0x20 /**< RTC clock stopped */
#define PCF85063_CTRL1_12_24 0x02 /**< 1 = 12-hour mode */
#define PCF85063_SECONDS_OS  0x80 /**< oscillator stopped: time invalid */

/**
 * @brief Decode the time registers to a UTC epoch.
 *
 * @param register_values Registers 0x04..0x0A as read from the chip.
 * @param out Seconds since 1970-01-01 UTC.
 * @return false if the OS flag is set or a field is out of range (time not usable).
 */
bool pcf85063_decode(const uint8_t register_values[PCF85063_TIME_LEN], time_t *out);

/**
 * @brief Encode a UTC epoch into the time registers (OS flag clear).
 *
 * @param timestamp Seconds since 1970-01-01 UTC, within 2000-01-01 .. 2099-12-31.
 * @param register_values Registers 0x04..0x0A to write to the chip.
 * @return false if @p timestamp is outside the chip's range (nothing encoded).
 */
bool pcf85063_encode(time_t timestamp, uint8_t register_values[PCF85063_TIME_LEN]);
