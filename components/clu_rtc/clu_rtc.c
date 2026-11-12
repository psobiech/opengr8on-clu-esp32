/**
 * @file clu_rtc.c
 * @brief PCF85063 RTC over the ESP-IDF I2C master driver.
 */
#include "clu_rtc.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "pcf85063.h"

static const char *TAG = "clu_rtc";

#define I2C_HZ      100000
#define I2C_TIMEOUT 50 // ms

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

bool clu_rtc_init(int sda_gpio, int scl_gpio)
{
    if (s_dev) {
        return true;
    }
    if (sda_gpio < 0 || scl_gpio < 0) {
        return false;
    }
    if (!s_bus) {
        i2c_master_bus_config_t bc = {
            .i2c_port = -1, // any free controller
            .sda_io_num = sda_gpio,
            .scl_io_num = scl_gpio,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true, // the board has 10K pull-ups too
        };
        if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) {
            ESP_LOGE(TAG, "I2C bus (SDA %d, SCL %d) failed", sda_gpio, scl_gpio);
            return false;
        }
    }
    if (i2c_master_probe(s_bus, PCF85063_I2C_ADDR, I2C_TIMEOUT) != ESP_OK) {
        ESP_LOGW(TAG, "no PCF85063 at 0x%02x (SDA %d, SCL %d)", PCF85063_I2C_ADDR, sda_gpio, scl_gpio);
        return false;
    }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_I2C_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(s_bus, &dc, &dev) != ESP_OK) {
        return false;
    }
    // The time registers are decoded as 24-hour and only count while STOP is clear.
    uint8_t reg = PCF85063_REG_CONTROL1, c1;
    if (i2c_master_transmit_receive(dev, &reg, 1, &c1, 1, I2C_TIMEOUT) != ESP_OK) {
        i2c_master_bus_rm_device(dev);
        return false;
    }
    if (c1 & (PCF85063_CTRL1_STOP | PCF85063_CTRL1_12_24)) {
        uint8_t w[2] = {PCF85063_REG_CONTROL1, c1 & ~(PCF85063_CTRL1_STOP | PCF85063_CTRL1_12_24)};
        i2c_master_transmit(dev, w, sizeof w, I2C_TIMEOUT);
        ESP_LOGW(TAG, "Control_1 0x%02x -> 0x%02x (running, 24-hour)", c1, w[1]);
    }
    s_dev = dev;
    return true;
}

bool clu_rtc_get(time_t *out)
{
    if (!s_dev) {
        return false;
    }
    uint8_t reg = PCF85063_REG_SECONDS, r[PCF85063_TIME_LEN];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, r, sizeof r, I2C_TIMEOUT) != ESP_OK) {
        ESP_LOGW(TAG, "read failed");
        return false;
    }
    time_t t;
    if (!pcf85063_decode(r, &t) || t < CLU_RTC_MIN_VALID) {
        return false;
    }
    *out = t;
    return true;
}

bool clu_rtc_set(time_t t)
{
    uint8_t w[1 + PCF85063_TIME_LEN] = {PCF85063_REG_SECONDS};
    if (!s_dev || t < CLU_RTC_MIN_VALID || !pcf85063_encode(t, w + 1)) {
        return false;
    }
    if (i2c_master_transmit(s_dev, w, sizeof w, I2C_TIMEOUT) != ESP_OK) {
        ESP_LOGW(TAG, "write failed");
        return false;
    }
    return true;
}
