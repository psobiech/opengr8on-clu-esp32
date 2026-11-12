/**
 * @file clu_rtc.h
 * @brief Battery-backed RTC on I2C: PCF85063 (Waveshare ESP32-S3-RS485-CAN: SDA IO39, SCL IO38)
 * or DS3231 (KinCony F8, KC868-A8v3: SDA IO8, SCL IO18, shared with the IO expander).
 *
 * Holds UTC. Keeps time through a power cut only with a backup cell; without one the chip
 * reports its time as invalid after power-up.
 */
#pragma once

#include <stdbool.h>
#include <time.h>

/** @brief Earliest time taken as a set clock (2024-01-01 UTC): anything before is "not set". */
#define CLU_RTC_MIN_VALID ((time_t)1704067200)

/** @brief RTC chip. */
typedef enum {
    CLU_RTC_PCF85063, /**< NXP PCF85063A/TP, I2C 0x51 */
    CLU_RTC_DS3231,   /**< Maxim DS3231, I2C 0x68 */
} clu_rtc_chip_t;

/**
 * @brief Get the (shared) I2C bus and probe the chip; 24-hour mode and a running clock are
 * enforced (PCF85063: STOP / 12_24 cleared; DS3231: oscillator kept running on battery).
 *
 * Idempotent once the chip answered. A pin < 0 means no RTC: returns false.
 *
 * @return true if the chip answered.
 */
bool clu_rtc_init(clu_rtc_chip_t chip, int sda_gpio, int scl_gpio);

/**
 * @brief Read the RTC.
 *
 * @param out UTC epoch.
 * @return false if there is no chip, the read failed, the chip lost power (PCF85063 OS /
 *         DS3231 OSF flag) or the time is before CLU_RTC_MIN_VALID.
 */
bool clu_rtc_get(time_t *out);

/**
 * @brief Set the RTC (clears the OS / OSF flag). Thread-safe (I2C bus lock).
 *
 * @param t UTC epoch; ignored if before CLU_RTC_MIN_VALID.
 * @return true if written.
 */
bool clu_rtc_set(time_t t);
