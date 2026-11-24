/**
 * @file clu_localio.c
 * @brief On-board IO glue: module serials (NVS / menuconfig), one PCF8575 (8 relays + 8 inputs,
 * module 1), optional ESP32 GPIO inputs (CLU_LOCAL_IO_GPIO_INPUTS, DIN of module 2), tfbus_uart hook.
 */
#include "clu_localio.h"

#include "sdkconfig.h"

#if CONFIG_CLU_LOCAL_IO

#include <stdlib.h>

#include "clu_crypto.h"
#include "clu_i2c.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "nvs.h"
#include "tfbus_uart.h"

static const char *TAG = "localio";

#define NVS_NS      "localio"
#define I2C_HZ      400000
#define I2C_TIMEOUT 20 // ms
#define GPIO_INPUTS_MAX 8

#ifdef CONFIG_CLU_LOCAL_IO_SN2
#define LOCAL_IO_SN2 CONFIG_CLU_LOCAL_IO_SN2
#else
#define LOCAL_IO_SN2 ""
#endif

static i2c_master_dev_handle_t s_dev;
static bool s_write_ok = true, s_read_ok = true; // last result, to log only changes
static gpio_num_t s_gpio_inputs[GPIO_INPUTS_MAX];   // module 2 DIN1.. pins
static int s_gpio_input_count;

static uint8_t read_gpio_inputs(void);
static int setup_gpio_inputs(const char *pin_list);

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
    const uint8_t output_bytes[2] = {out, 0xff}; // P00-P07 relays, P10-P17 inputs
#else
    const uint8_t output_bytes[2] = {0xff, out}; // P00-P07 inputs, P10-P17 relays
#endif
    esp_err_t err = i2c_master_transmit(s_dev, output_bytes, sizeof output_bytes, I2C_TIMEOUT);
    track(&s_write_ok, err == ESP_OK, "write", err);
    return err == ESP_OK;
}

/** @brief localio_hw_t::read: the 8 PCF8575 inputs (bits 0-7) and the GPIO inputs (bits 8-15). */
static bool hw_read(uint16_t *active, void *ctx)
{
    uint8_t input_bytes[2];
    esp_err_t err = i2c_master_receive(s_dev, input_bytes, sizeof input_bytes, I2C_TIMEOUT);
    track(&s_read_ok, err == ESP_OK, "read", err);
    if (err != ESP_OK) {
        return false;
    }
#if CONFIG_CLU_LOCAL_IO_OUT_P0
    uint8_t in = input_bytes[1];
#else
    uint8_t in = input_bytes[0];
#endif
    uint8_t expander_inputs = (uint8_t)(CONFIG_CLU_LOCAL_IO_IN_ACTIVE_LOW ? ~in : in);
    *active = (uint16_t)(expander_inputs | read_gpio_inputs() << 8);
    return true;
}

/** @brief GPIO inputs, bit i = s_gpio_inputs[i] pulled low. */
static uint8_t read_gpio_inputs(void)
{
    uint8_t active_inputs = 0;
    for (int i = 0; i < s_gpio_input_count; i++) {
        if (gpio_get_level(s_gpio_inputs[i]) == 0) {
            active_inputs |= (uint8_t)(1u << i);
        }
    }
    return active_inputs;
}

static const localio_hw_t s_hw = {.write = hw_write, .read = hw_read};

/**
 * @brief Module serial: menuconfig value if set (stored), else the one in NVS, else a new random
 * one (stored). Never 0.
 */
static uint32_t serial(nvs_handle_t nvs_handle, const char *key, const char *cfg)
{
    uint32_t serial_number = 0;
    bool have = nvs_get_u32(nvs_handle, key, &serial_number) == ESP_OK && serial_number != 0;
    if (cfg[0]) {
        uint32_t configured_serial = (uint32_t)strtoul(cfg, NULL, 10);
        if (configured_serial != 0) {
            if (!have || serial_number != configured_serial) {
                nvs_set_u32(nvs_handle, key, configured_serial);
            }
            return configured_serial;
        }
        ESP_LOGE(TAG, "serial \"%s\" is not a number, ignored", cfg);
    }
    if (have) {
        return serial_number;
    }
    do {
        clu_random((uint8_t *)&serial_number, sizeof serial_number);
        serial_number &= 0x7FFFFFFF;
    } while (serial_number == 0);
    nvs_set_u32(nvs_handle, key, serial_number);
    return serial_number;
}

void clu_localio_start(void)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: on-board IO disabled");
        return;
    }
    s_gpio_input_count = setup_gpio_inputs(CONFIG_CLU_LOCAL_IO_GPIO_INPUTS);
    const int module_count = s_gpio_input_count > 0 ? 2 : 1;
    uint32_t serial_numbers[LOCALIO_MAX_MODULES];
    serial_numbers[0] = serial(nvs_handle, "sn1", CONFIG_CLU_LOCAL_IO_SN);
    if (module_count == 2) {
        serial_numbers[1] = serial(nvs_handle, "sn2", LOCAL_IO_SN2);
    }
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    i2c_master_bus_handle_t bus = clu_i2c_bus(CONFIG_CLU_LOCAL_IO_SDA_GPIO, CONFIG_CLU_LOCAL_IO_SCL_GPIO);
    if (!bus) {
        ESP_LOGE(TAG, "no I2C bus (SDA %d, SCL %d): on-board IO disabled", CONFIG_CLU_LOCAL_IO_SDA_GPIO,
                 CONFIG_CLU_LOCAL_IO_SCL_GPIO);
        return;
    }
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_CLU_LOCAL_IO_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &device_config, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCF8575 0x%02x: %s: on-board IO disabled", CONFIG_CLU_LOCAL_IO_ADDR, esp_err_to_name(err));
        return;
    }
    if (i2c_master_probe(bus, CONFIG_CLU_LOCAL_IO_ADDR, I2C_TIMEOUT) != ESP_OK) {
        // Kept anyway: the module then shows up offline until the expander answers.
        ESP_LOGW(TAG, "no PCF8575 at 0x%02x", CONFIG_CLU_LOCAL_IO_ADDR);
    }

    localio_setup(serial_numbers, module_count, &s_hw); // relays off, input pins high
    tfbus_uart_add_local(localio_frame);
    ESP_LOGI(TAG, "on-board IO: IO_MODULE_DIN_8 sn=%lu (PCF8575 0x%02x)", (unsigned long)serial_numbers[0],
             CONFIG_CLU_LOCAL_IO_ADDR);
    if (module_count == 2) {
        ESP_LOGI(TAG, "on-board IO: IO_MODULE_DIN_8 sn=%lu (%d GPIO inputs: %s)", (unsigned long)serial_numbers[1],
                 s_gpio_input_count, CONFIG_CLU_LOCAL_IO_GPIO_INPUTS);
    }
}

/**
 * @brief Parse @p pin_list ("48,47,21") into s_gpio_inputs and make the pins inputs with pull-up.
 * @return Number of pins; 0 if the list is empty or invalid (no second module).
 */
static int setup_gpio_inputs(const char *pin_list)
{
    int pin_count = 0;
    uint64_t pin_mask = 0;
    const char *cursor = pin_list;
    while (*cursor == ' ') {
        cursor++;
    }
    while (*cursor) {
        char *after_number;
        long pin_number = strtol(cursor, &after_number, 10);
        if (after_number == cursor || !GPIO_IS_VALID_GPIO(pin_number) || pin_count == GPIO_INPUTS_MAX) {
            ESP_LOGE(TAG, "GPIO inputs \"%s\": invalid pin list, GPIO input module disabled", pin_list);
            return 0;
        }
        s_gpio_inputs[pin_count++] = (gpio_num_t)pin_number;
        pin_mask |= 1ULL << pin_number;
        cursor = after_number;
        while (*cursor == ',' || *cursor == ' ') {
            cursor++;
        }
    }
    if (pin_count == 0) {
        return 0;
    }
    const gpio_config_t input_config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&input_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO inputs \"%s\": %s, GPIO input module disabled", pin_list, esp_err_to_name(err));
        return 0;
    }
    return pin_count;
}

#else

void clu_localio_start(void) {}

#endif // CONFIG_CLU_LOCAL_IO
