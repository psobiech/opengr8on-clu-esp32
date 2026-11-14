/**
 * @file clu_i2c.c
 * @brief Shared I2C master buses (one per SDA/SCL pair).
 */
#include "clu_i2c.h"

#include <stdbool.h>

#include "esp_log.h"

static const char *TAG = "clu_i2c";

#define MAX_BUSES 2 // I2C controllers on the ESP32 / ESP32-S3

static struct {
    int sda, scl;
    i2c_master_bus_handle_t bus;
} s_bus[MAX_BUSES];
static int s_nbus;

i2c_master_bus_handle_t clu_i2c_bus(int sda_gpio, int scl_gpio)
{
    if (sda_gpio < 0 || scl_gpio < 0) {
        return NULL;
    }
    for (int i = 0; i < s_nbus; i++) {
        if (s_bus[i].sda == sda_gpio && s_bus[i].scl == scl_gpio) {
            return s_bus[i].bus;
        }
        if (s_bus[i].sda == sda_gpio || s_bus[i].scl == scl_gpio || s_bus[i].sda == scl_gpio ||
            s_bus[i].scl == sda_gpio) {
            ESP_LOGE(TAG, "SDA %d / SCL %d: pin already used by the bus on SDA %d / SCL %d", sda_gpio, scl_gpio,
                     s_bus[i].sda, s_bus[i].scl);
            return NULL;
        }
    }
    if (s_nbus >= MAX_BUSES) {
        ESP_LOGE(TAG, "SDA %d / SCL %d: no free I2C controller", sda_gpio, scl_gpio);
        return NULL;
    }
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = -1, // any free controller
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, // boards usually have external pull-ups too
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus on SDA %d / SCL %d: %s", sda_gpio, scl_gpio, esp_err_to_name(err));
        return NULL;
    }
    s_bus[s_nbus].sda = sda_gpio;
    s_bus[s_nbus].scl = scl_gpio;
    s_bus[s_nbus].bus = bus;
    s_nbus++;
    return bus;
}
