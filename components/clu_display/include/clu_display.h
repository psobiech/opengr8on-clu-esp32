/**
 * @file clu_display.h
 * @brief 128x64 SSD1306 OLED on I2C (shared bus, clu_i2c) as 8 rows of 21 characters.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "display_text.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Text rows on the screen (one SSD1306 page each). */
#define CLU_DISPLAY_ROWS    8

/** @brief Characters per row; longer rows are cut off. */
#define CLU_DISPLAY_COLUMNS DISPLAY_TEXT_COLUMNS

/**
 * @brief Probe and set up the display, switch it on.
 *
 * Call while the drivers are set up (boot): the shared bus is created here on first use.
 *
 * @param sda_gpio    I2C SDA.
 * @param scl_gpio    I2C SCL.
 * @param i2c_address 7-bit address (0x3c or 0x3d).
 * @return false if there is no display at that address (logged) or it could not be set up.
 */
bool clu_display_init(int sda_gpio, int scl_gpio, uint8_t i2c_address);

/**
 * @brief Show these rows. Only rows that differ from what the display shows are sent.
 *
 * Each row is its own short I2C transfer, so other devices on the shared bus wait at most one row.
 * No-op before a successful clu_display_init(). Call from one task only.
 *
 * @param row_texts CLU_DISPLAY_ROWS NUL-terminated rows.
 */
void clu_display_show(const char *const row_texts[CLU_DISPLAY_ROWS]);

#ifdef __cplusplus
}
#endif
