/**
 * @file clu_rtc.c
 * @brief PCF85063 / DS3231 RTC over the ESP-IDF I2C master driver (shared bus, clu_i2c).
 */
#include "clu_rtc.h"

#include "clu_i2c.h"
#include "ds3231.h"
#include "esp_log.h"
#include "pcf85063.h"

static const char *TAG = "clu_rtc";

#define I2C_HZ      100000
#define I2C_TIMEOUT 50 // ms

static clu_rtc_chip_t s_type; // chip of s_dev
static i2c_master_dev_handle_t s_dev;

/** @brief Read @p n registers from @p reg. */
static bool rd(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, n, I2C_TIMEOUT) == ESP_OK;
}

/** @brief Write one register. */
static bool wr(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t v)
{
    const uint8_t w[2] = {reg, v};
    return i2c_master_transmit(dev, w, sizeof w, I2C_TIMEOUT) == ESP_OK;
}

/** @brief Chip setup after the probe: a clock that runs (and keeps running on battery), 24-hour mode. */
static bool chip_setup(clu_rtc_chip_t chip, i2c_master_dev_handle_t dev)
{
    uint8_t c;
    if (chip == CLU_RTC_DS3231) {
        // EOSC set: the oscillator would stop on battery. 12-hour mode is cleared by every set.
        if (!rd(dev, DS3231_REG_CONTROL, &c, 1)) {
            return false;
        }
        if (c & DS3231_CONTROL_EOSC) {
            wr(dev, DS3231_REG_CONTROL, c & ~DS3231_CONTROL_EOSC);
            ESP_LOGW(TAG, "DS3231 Control 0x%02x -> 0x%02x (oscillator on battery)", c,
                     c & ~DS3231_CONTROL_EOSC);
        }
        return true;
    }
    // PCF85063: the time registers are decoded as 24-hour and only count while STOP is clear.
    if (!rd(dev, PCF85063_REG_CONTROL1, &c, 1)) {
        return false;
    }
    if (c & (PCF85063_CTRL1_STOP | PCF85063_CTRL1_12_24)) {
        uint8_t v = c & ~(PCF85063_CTRL1_STOP | PCF85063_CTRL1_12_24);
        wr(dev, PCF85063_REG_CONTROL1, v);
        ESP_LOGW(TAG, "Control_1 0x%02x -> 0x%02x (running, 24-hour)", c, v);
    }
    return true;
}

bool clu_rtc_init(clu_rtc_chip_t chip, int sda_gpio, int scl_gpio)
{
    if (s_dev) {
        return true;
    }
    i2c_master_bus_handle_t bus = clu_i2c_bus(sda_gpio, scl_gpio);
    if (!bus) {
        return false;
    }
    const uint8_t addr = chip == CLU_RTC_DS3231 ? DS3231_I2C_ADDR : PCF85063_I2C_ADDR;
    const char *name = chip == CLU_RTC_DS3231 ? "DS3231" : "PCF85063";
    if (i2c_master_probe(bus, addr, I2C_TIMEOUT) != ESP_OK) {
        ESP_LOGW(TAG, "no %s at 0x%02x (SDA %d, SCL %d)", name, addr, sda_gpio, scl_gpio);
        return false;
    }
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_HZ,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) {
        return false;
    }
    if (!chip_setup(chip, dev)) {
        i2c_master_bus_rm_device(dev);
        return false;
    }
    s_type = chip;
    s_dev = dev;
    return true;
}

bool clu_rtc_get(time_t *out)
{
    if (!s_dev) {
        return false;
    }
    time_t t;
    bool ok;
    if (s_type == CLU_RTC_DS3231) {
        uint8_t r[DS3231_REG_STATUS + 1]; // time registers .. Status (OSF)
        if (!rd(s_dev, DS3231_REG_SECONDS, r, sizeof r)) {
            ESP_LOGW(TAG, "read failed");
            return false;
        }
        ok = !(r[DS3231_REG_STATUS] & DS3231_STATUS_OSF) && ds3231_decode(r, &t);
    } else {
        uint8_t r[PCF85063_TIME_LEN];
        if (!rd(s_dev, PCF85063_REG_SECONDS, r, sizeof r)) {
            ESP_LOGW(TAG, "read failed");
            return false;
        }
        ok = pcf85063_decode(r, &t);
    }
    if (!ok || t < CLU_RTC_MIN_VALID) {
        return false;
    }
    *out = t;
    return true;
}

bool clu_rtc_set(time_t t)
{
    uint8_t w[1 + DS3231_TIME_LEN]; // both chips: register address + 7 time registers
    if (!s_dev || t < CLU_RTC_MIN_VALID) {
        return false;
    }
    if (s_type == CLU_RTC_DS3231) {
        w[0] = DS3231_REG_SECONDS;
        if (!ds3231_encode(t, w + 1)) {
            return false;
        }
    } else {
        w[0] = PCF85063_REG_SECONDS;
        if (!pcf85063_encode(t, w + 1)) { // OS cleared with the seconds
            return false;
        }
    }
    if (i2c_master_transmit(s_dev, w, sizeof w, I2C_TIMEOUT) != ESP_OK) {
        ESP_LOGW(TAG, "write failed");
        return false;
    }
    if (s_type == CLU_RTC_DS3231) {
        // Time valid again: clear OSF, keep the other Status bits (EN32kHz).
        uint8_t st;
        if (!rd(s_dev, DS3231_REG_STATUS, &st, 1) ||
            ((st & DS3231_STATUS_OSF) && !wr(s_dev, DS3231_REG_STATUS, st & ~DS3231_STATUS_OSF))) {
            ESP_LOGW(TAG, "DS3231 OSF clear failed");
            return false;
        }
    }
    return true;
}
