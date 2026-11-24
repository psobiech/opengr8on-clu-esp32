/**
 * @file display_text.h
 * @brief Text rows for a 128x64 monochrome OLED (SSD1306 page layout), independent of the hardware.
 *
 * One text row is one SSD1306 page: 8 pixels high, one byte per pixel column, bit 0 at the top.
 * Characters are 5x7 glyphs in 6-pixel cells (one blank column between them).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Pixel columns of one row (= bytes of one SSD1306 page). */
#define DISPLAY_TEXT_WIDTH 128

/** @brief Characters per row: 6-pixel cells, the last 2 pixel columns stay blank. */
#define DISPLAY_TEXT_COLUMNS 21

/**
 * @brief Render one text row into one page.
 *
 * Characters past DISPLAY_TEXT_COLUMNS are cut off; characters outside printable ASCII
 * (0x20-0x7e) are shown as '?'. The rest of the row is blank.
 *
 * @param text       NUL-terminated text.
 * @param page_bytes Output: DISPLAY_TEXT_WIDTH column bytes.
 */
void display_text_render_row(const char *text, uint8_t page_bytes[DISPLAY_TEXT_WIDTH]);

#ifdef __cplusplus
}
#endif
