/**
 * @file clu_display.c
 * @brief SSD1306 OLED over the ESP-IDF esp_lcd driver on a shared I2C bus (clu_i2c).
 */
#include "clu_display.h"

#include <string.h>

#include "clu_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"

static const char *TAG = "clu_display";

#define I2C_HZ             400000
#define I2C_PROBE_TIMEOUT  50 // ms
#define DISPLAY_HEIGHT     64
#define ROW_HEIGHT         8 // pixels of one SSD1306 page

static esp_lcd_panel_handle_t s_panel;
static uint8_t s_shown_pages[CLU_DISPLAY_ROWS][DISPLAY_TEXT_WIDTH]; // what the display shows
static bool s_page_shown[CLU_DISPLAY_ROWS]; // s_shown_pages[row] is valid (false: unknown, resend)

static bool panel_setup(esp_lcd_panel_handle_t panel);

bool clu_display_init(int sda_gpio, int scl_gpio, uint8_t i2c_address)
{
    if (s_panel) {
        return true;
    }
    i2c_master_bus_handle_t bus = clu_i2c_bus(sda_gpio, scl_gpio);
    if (!bus) {
        return false;
    }
    if (i2c_master_probe(bus, i2c_address, I2C_PROBE_TIMEOUT) != ESP_OK) {
        ESP_LOGW(TAG, "no SSD1306 at 0x%02x (SDA %d, SCL %d)", i2c_address, sda_gpio, scl_gpio);
        return false;
    }
    const esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = i2c_address,
        .scl_speed_hz = I2C_HZ,
        .control_phase_bytes = 1, // control byte before commands / data (SSD1306 datasheet)
        .dc_bit_offset = 6,       // its D/C# bit
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t panel_io;
    if (esp_lcd_new_panel_io_i2c(bus, &io_config, &panel_io) != ESP_OK) {
        return false;
    }
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {.height = DISPLAY_HEIGHT};
    const esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd1306_config,
    };
    esp_lcd_panel_handle_t panel;
    if (esp_lcd_new_panel_ssd1306(panel_io, &panel_config, &panel) != ESP_OK) {
        esp_lcd_panel_io_del(panel_io);
        return false;
    }
    if (!panel_setup(panel)) {
        ESP_LOGE(TAG, "SSD1306 at 0x%02x: setup failed", i2c_address);
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(panel_io);
        return false;
    }
    s_panel = panel;
    return true;
}

void clu_display_show(const char *const row_texts[CLU_DISPLAY_ROWS])
{
    if (!s_panel) {
        return;
    }
    uint8_t page_bytes[DISPLAY_TEXT_WIDTH];
    for (int row = 0; row < CLU_DISPLAY_ROWS; row++) {
        display_text_render_row(row_texts[row], page_bytes);
        if (s_page_shown[row] && memcmp(page_bytes, s_shown_pages[row], sizeof page_bytes) == 0) {
            continue;
        }
        const int y_start = row * ROW_HEIGHT;
        s_page_shown[row] = esp_lcd_panel_draw_bitmap(s_panel, 0, y_start, DISPLAY_TEXT_WIDTH, y_start + ROW_HEIGHT,
                                                      page_bytes) == ESP_OK;
        if (s_page_shown[row]) {
            memcpy(s_shown_pages[row], page_bytes, sizeof page_bytes);
        }
    }
}

/**
 * @brief Init sequence, orientation, display on. The RAM content is unknown until every row was
 * sent once (s_page_shown all false).
 */
static bool panel_setup(esp_lcd_panel_handle_t panel)
{
    return esp_lcd_panel_reset(panel) == ESP_OK && esp_lcd_panel_init(panel) == ESP_OK &&
           // Segment remap + COM scan reversed: upright on the usual 0.96" modules (the IDF init
           // leaves both off, which shows the picture rotated by 180 degrees).
           esp_lcd_panel_mirror(panel, true, true) == ESP_OK && esp_lcd_panel_disp_on_off(panel, true) == ESP_OK;
}
