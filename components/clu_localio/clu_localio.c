/**
 * @file clu_localio.c
 * @brief On-board IO glue: module serial (NVS / menuconfig), one PCF8575 (8 relays + 8 inputs),
 * tfbus_uart hook.
 */
#include "clu_localio.h"

#include "sdkconfig.h"

#if CONFIG_CLU_LOCAL_IO

#include <stdlib.h>

#include "clu_crypto.h"
#include "clu_i2c.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "nvs.h"
#include "tfbus_uart.h"

static const char *TAG = "localio";

#define NVS_NS      "localio"
#define I2C_HZ      400000
#define I2C_TIMEOUT 20 // ms

static i2c_master_dev_handle_t s_dev;
static bool s_write_ok = true, s_read_ok = true; // last result, to log only changes

/** @brief Log the expander failing / recovering once per change. */
static void track(bool *ok, bool now, const char *what, esp_err_t err)
{
    if (*ok != now) {
        if (now) {
            ESP_LOGI(TAG, "PCF8575 %s OK again", what);
        } else {
            ESP_LOGE(TAG, "PCF8575 %s: %s", what, esp_err_to_name(err));
        }
        *ok = now;
    }
}

/**
 * @brief localio_hw_t::write: the 8 relay outputs (bits 0-7).
 *
 * One PCF8575 holds relays and inputs. Its pins are quasi-bidirectional: the input half is always
 * written high so it can be read.
 */
static bool hw_write(uint16_t on, void *ctx)
{
    uint8_t out = (uint8_t)(CONFIG_CLU_LOCAL_IO_OUT_ACTIVE_LOW ? ~on : on);
#if CONFIG_CLU_LOCAL_IO_OUT_P0
    const uint8_t b[2] = {out, 0xff}; // P00-P07 relays, P10-P17 inputs
#else
    const uint8_t b[2] = {0xff, out}; // P00-P07 inputs, P10-P17 relays
#endif
    esp_err_t err = i2c_master_transmit(s_dev, b, sizeof b, I2C_TIMEOUT);
    track(&s_write_ok, err == ESP_OK, "write", err);
    return err == ESP_OK;
}

/** @brief localio_hw_t::read: the 8 inputs (bits 0-7). */
static bool hw_read(uint16_t *active, void *ctx)
{
    uint8_t b[2];
    esp_err_t err = i2c_master_receive(s_dev, b, sizeof b, I2C_TIMEOUT);
    track(&s_read_ok, err == ESP_OK, "read", err);
    if (err != ESP_OK) {
        return false;
    }
#if CONFIG_CLU_LOCAL_IO_OUT_P0
    uint8_t in = b[1];
#else
    uint8_t in = b[0];
#endif
    *active = (uint8_t)(CONFIG_CLU_LOCAL_IO_IN_ACTIVE_LOW ? ~in : in);
    return true;
}

static const localio_hw_t s_hw = {.write = hw_write, .read = hw_read};

/**
 * @brief Module serial: menuconfig value if set (stored), else the one in NVS, else a new random
 * one (stored). Never 0.
 */
static uint32_t serial(nvs_handle_t h, const char *key, const char *cfg)
{
    uint32_t sn = 0;
    bool have = nvs_get_u32(h, key, &sn) == ESP_OK && sn != 0;
    if (cfg[0]) {
        uint32_t c = (uint32_t)strtoul(cfg, NULL, 10);
        if (c != 0) {
            if (!have || sn != c) {
                nvs_set_u32(h, key, c);
            }
            return c;
        }
        ESP_LOGE(TAG, "serial \"%s\" is not a number, ignored", cfg);
    }
    if (have) {
        return sn;
    }
    do {
        clu_random((uint8_t *)&sn, sizeof sn);
        sn &= 0x7FFFFFFF;
    } while (sn == 0);
    nvs_set_u32(h, key, sn);
    return sn;
}

void clu_localio_start(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: on-board IO disabled");
        return;
    }
    const uint32_t sn = serial(h, "sn1", CONFIG_CLU_LOCAL_IO_SN);
    nvs_commit(h);
    nvs_close(h);

    i2c_master_bus_handle_t bus = clu_i2c_bus(CONFIG_CLU_LOCAL_IO_SDA_GPIO, CONFIG_CLU_LOCAL_IO_SCL_GPIO);
    if (!bus) {
        ESP_LOGE(TAG, "no I2C bus (SDA %d, SCL %d): on-board IO disabled", CONFIG_CLU_LOCAL_IO_SDA_GPIO,
                 CONFIG_CLU_LOCAL_IO_SCL_GPIO);
        return;
    }
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_CLU_LOCAL_IO_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCF8575 0x%02x: %s: on-board IO disabled", CONFIG_CLU_LOCAL_IO_ADDR, esp_err_to_name(err));
        return;
    }
    if (i2c_master_probe(bus, CONFIG_CLU_LOCAL_IO_ADDR, I2C_TIMEOUT) != ESP_OK) {
        // Kept anyway: the module then shows up offline until the expander answers.
        ESP_LOGW(TAG, "no PCF8575 at 0x%02x", CONFIG_CLU_LOCAL_IO_ADDR);
    }

    localio_setup(&sn, 1, &s_hw); // relays off, input pins high
    tfbus_uart_add_local(localio_frame);
    ESP_LOGI(TAG, "on-board IO: IO_MODULE_DIN_8 sn=%lu (PCF8575 0x%02x)", (unsigned long)sn,
             CONFIG_CLU_LOCAL_IO_ADDR);
}

#else

void clu_localio_start(void) {}

#endif // CONFIG_CLU_LOCAL_IO
