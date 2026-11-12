/**
 * @file ds3231.h
 * @brief DS3231 real-time clock: register map and time register encoding (no I/O).
 *
 * The seven time registers (Seconds .. Year, 0x00..0x06) hold UTC in BCD, 24-hour mode,
 * years 2000..2099 (Century bit clear). The chip sets OSF (Status bit 7) when its oscillator
 * stopped (power lost with no backup cell): the stored time is then invalid until written again
 * and OSF cleared.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define DS3231_I2C_ADDR     0x68
#define DS3231_REG_SECONDS  0x00
#define DS3231_REG_CONTROL  0x0E
#define DS3231_REG_STATUS   0x0F
#define DS3231_TIME_LEN     7 /**< Seconds, Minutes, Hours, Day (1-7), Date, Month/Century, Year */

#define DS3231_CONTROL_EOSC  0x80 /**< 1 = oscillator stops on battery power */
#define DS3231_STATUS_OSF    0x80 /**< oscillator stopped: time invalid */
#define DS3231_HOURS_12H     0x40 /**< 1 = 12-hour mode */
#define DS3231_MONTH_CENTURY 0x80 /**< year rolled over past 2099 */

/**
 * @brief Decode the time registers to a UTC epoch (OSF is in the Status register, not checked here).
 *
 * @param r   Registers 0x00..0x06 as read from the chip.
 * @param out Seconds since 1970-01-01 UTC.
 * @return false if the chip is in 12-hour mode, past 2099 or a field is out of range.
 */
bool ds3231_decode(const uint8_t r[DS3231_TIME_LEN], time_t *out);

/**
 * @brief Encode a UTC epoch into the time registers (24-hour mode, Century clear).
 *
 * @param t Seconds since 1970-01-01 UTC, within 2000-01-01 .. 2099-12-31.
 * @param r Registers 0x00..0x06 to write to the chip.
 * @return false if @p t is outside the chip's range (nothing encoded).
 */
bool ds3231_encode(time_t t, uint8_t r[DS3231_TIME_LEN]);
