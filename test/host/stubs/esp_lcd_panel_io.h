// Host stub: the esp_lcd panel IO API used by clu_display.c (the test implements it).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
typedef struct esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;
typedef struct esp_lcd_panel_t *esp_lcd_panel_handle_t;
typedef struct {
    uint32_t dev_addr;
    size_t control_phase_bytes;
    unsigned int dc_bit_offset;
    int lcd_cmd_bits;
    int lcd_param_bits;
    uint32_t scl_speed_hz;
} esp_lcd_panel_io_i2c_config_t;
esp_err_t esp_lcd_new_panel_io_i2c(i2c_master_bus_handle_t bus, const esp_lcd_panel_io_i2c_config_t *io_config,
                                   esp_lcd_panel_io_handle_t *ret_io);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io);
