// Host stub: the esp_lcd SSD1306 panel driver used by clu_display.c (the test implements it).
#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
typedef struct {
    uint8_t height;
} esp_lcd_panel_ssd1306_config_t;
esp_err_t esp_lcd_new_panel_ssd1306(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);
