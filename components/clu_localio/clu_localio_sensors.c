/**
 * @file clu_localio_sensors.c
 * @brief On-board analog inputs (ADC1, CLU_LOCAL_IO_ANALOG_INPUTS) and 1-Wire temperature sensors
 * on up to 4 buses (localio_onewire_pins.h) measured by a low-priority task and served by
 * localio_sensors.c as an ANALOG_DIN module + one ONE_WIRE module per sensor (all buses behind its
 * one ONE_WIRE_HUB).
 *
 * The task samples the inputs every SAMPLE_PERIOD_MS, starts a temperature conversion on all
 * sensors of every bus every ONEWIRE_PERIOD_MS and reads them CONVERSION_MS later (12-bit DS18B20:
 * 750 ms). A search covers every bus and remembers which bus each sensor is on.
 * A search request (bus RESET, RefreshSensors) wakes it at once.
 */
#include "clu_localio.h"

#include "sdkconfig.h"

#if CONFIG_CLU_LOCAL_IO

#include <stdlib.h>
#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "localio_onewire_pins.h"
#include "localio_sensors.h"
#include "onewire_bus.h"
#include "onewire_device.h"
#include "tfbus_uart.h"
#include "tfbus_vmod.h"

static const char *TAG = "localio";

#define SAMPLE_PERIOD_MS     100
#define OVERSAMPLE_COUNT     16
#define ONEWIRE_PERIOD_MS    2000
#define CONVERSION_MS        800
#define ONEWIRE_CONVERT_T    0x44
#define ONEWIRE_READ_PAD     0xBE
#define ONEWIRE_SKIP_ROM     0xCC
#define ONEWIRE_MATCH_ROM    0x55
#define TASK_STACK           4096
#define TASK_PRIORITY        3

/** @brief One analog input: ADC1 channel and the factor from pin volts to the terminal value. */
typedef struct {
    int gpio;
    adc_channel_t channel;
    float factor;
    adc_cali_handle_t calibration; // NULL: no calibration scheme, linear estimate
} analog_input_t;

static adc_oneshot_unit_handle_t s_adc_unit;
static analog_input_t s_analog_inputs[TFBUS_ANALOG_INPUTS];
static int s_analog_input_count;
static uint32_t s_analog_serial_number; // 0 until the ANALOG_DIN module is on the bus
/** @brief Which bus a sensor was found on (MATCH ROM goes to that bus only). */
typedef struct {
    uint64_t rom_code;
    int bus_index;
} sensor_location_t;

static onewire_bus_handle_t s_onewire_buses[LOCALIO_ONEWIRE_BUSES_MAX];
static int s_onewire_bus_count;
static sensor_location_t s_sensor_locations[2 * LOCALIO_SENSORS_MAX]; // sensor task only
static int s_sensor_location_count;
static SemaphoreHandle_t s_wake;
static bool s_search_pending, s_search_replace; // set by the bus task, taken by the sensor task (atomics)

static int setup_analog_inputs(const char *input_list);
static int setup_onewire_buses(void);
static void request_search(bool replace_list);
static void sensor_task(void *arg);
static void sample_analog_inputs(void);
static void search_sensors(bool replace_list);
static void remember_location(uint64_t rom_code, int bus_index);
static void start_conversion(void);
static void read_sensors(void);
static bool read_scratchpad(uint64_t rom_code, uint8_t scratchpad[9]);
static int analog_millivolts(const analog_input_t *input, int raw_average);

bool clu_localio_sensors_start(uint32_t analog_serial_number)
{
    s_analog_input_count = setup_analog_inputs(CONFIG_CLU_LOCAL_IO_ANALOG_INPUTS);
    s_onewire_bus_count = setup_onewire_buses();
    bool have_onewire = s_onewire_bus_count > 0;
    if (s_analog_input_count == 0 && !have_onewire) {
        return false;
    }
    s_wake = xSemaphoreCreateBinary();
    localio_sensors_setup(analog_serial_number, have_onewire ? request_search : NULL);
    tfbus_uart_add_local(localio_sensors_frame);
    s_analog_serial_number = analog_serial_number;
    if (xTaskCreate(sensor_task, "localio_sens", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the sensor task: inputs and 1-Wire sensors read 0");
    }
    ESP_LOGI(TAG, "on-board IO: ANALOG_DIN sn=%lu (%d analog input(s), %d 1-Wire bus(es): %s)",
             (unsigned long)analog_serial_number, s_analog_input_count, s_onewire_bus_count, LOCALIO_ONEWIRE_GPIOS);
    return true;
}

bool clu_localio_sensors_channels(uint32_t serial_number, localio_channels_t *channels)
{
    if (s_analog_serial_number == 0 || serial_number != s_analog_serial_number) {
        return false;
    }
    *channels = (localio_channels_t){
        .analog_inputs = (uint8_t)((1u << s_analog_input_count) - 1),
        .onewire_hubs = s_onewire_bus_count > 0 ? 1 : 0,
    };
    return true;
}

/**
 * @brief Parse @p input_list ("5:1.51,7:1.51,6:6.667") into s_analog_inputs and set up ADC1 with
 * calibration. Entries: GPIO[:factor], factor 1 if left out.
 * @return Number of inputs; 0 if the list is empty or invalid.
 */
static int setup_analog_inputs(const char *input_list)
{
    int input_count = 0;
    const char *cursor = input_list;
    while (*cursor == ' ') {
        cursor++;
    }
    if (!*cursor) {
        return 0;
    }
    const adc_oneshot_unit_init_cfg_t unit_config = {.unit_id = ADC_UNIT_1};
    esp_err_t err = adc_oneshot_new_unit(&unit_config, &s_adc_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC1: %s, analog inputs disabled", esp_err_to_name(err));
        return 0;
    }
    while (*cursor) {
        char *after_number;
        long gpio = strtol(cursor, &after_number, 10);
        adc_unit_t unit;
        adc_channel_t channel;
        if (after_number == cursor || input_count == TFBUS_ANALOG_INPUTS ||
            adc_oneshot_io_to_channel((int)gpio, &unit, &channel) != ESP_OK || unit != ADC_UNIT_1) {
            ESP_LOGE(TAG, "analog inputs \"%s\": invalid list (ADC1 pins only, up to %d), analog inputs disabled",
                     input_list, TFBUS_ANALOG_INPUTS);
            return 0;
        }
        cursor = after_number;
        float factor = 1.0f;
        if (*cursor == ':') {
            factor = strtof(cursor + 1, &after_number);
            cursor = after_number;
        }
        analog_input_t *input = &s_analog_inputs[input_count++];
        *input = (analog_input_t){.gpio = (int)gpio, .channel = channel, .factor = factor};
        const adc_oneshot_chan_cfg_t channel_config = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        adc_oneshot_config_channel(s_adc_unit, channel, &channel_config);
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        const adc_cali_curve_fitting_config_t calibration_config = {
            .unit_id = ADC_UNIT_1, .chan = channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        err = adc_cali_create_scheme_curve_fitting(&calibration_config, &input->calibration);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        const adc_cali_line_fitting_config_t calibration_config = {
            .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        err = adc_cali_create_scheme_line_fitting(&calibration_config, &input->calibration);
#else
        err = ESP_ERR_NOT_SUPPORTED;
#endif
        if (err != ESP_OK) {
            input->calibration = NULL;
            ESP_LOGW(TAG, "analog input GPIO%ld: no ADC calibration (%s), values approximate", gpio, esp_err_to_name(err));
        }
        while (*cursor == ',' || *cursor == ' ') {
            cursor++;
        }
    }
    return input_count;
}

/** @brief Create a 1-Wire bus (RMT) on every configured pin; a pin that fails is logged and skipped.
 *  @return Number of buses in s_onewire_buses. */
static int setup_onewire_buses(void)
{
    int pins[LOCALIO_ONEWIRE_BUSES_MAX];
    int pin_count = localio_onewire_pins(pins, LOCALIO_ONEWIRE_BUSES_MAX);
    int bus_count = 0;
    for (int i = 0; i < pin_count; i++) {
        const onewire_bus_config_t bus_config = {.bus_gpio_num = pins[i], .flags = {.en_pull_up = true}};
        const onewire_bus_rmt_config_t rmt_config = {.max_rx_bytes = 10}; // ROM command + 8 ROM bytes, scratchpad 9
        esp_err_t err = onewire_new_bus_rmt(&bus_config, &rmt_config, &s_onewire_buses[bus_count]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "1-Wire bus on GPIO%d: %s, bus disabled", pins[i], esp_err_to_name(err));
            continue;
        }
        bus_count++;
    }
    return bus_count;
}

/** @brief ::localio_search_request_fn_t: flag the search and wake the task (never blocks). */
static void request_search(bool replace_list)
{
    if (replace_list) {
        __atomic_store_n(&s_search_replace, true, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&s_search_pending, true, __ATOMIC_RELEASE);
    xSemaphoreGive(s_wake);
}

/** @brief Measuring task: inputs every SAMPLE_PERIOD_MS, 1-Wire conversion cycle, searches on request. */
static void sensor_task(void *arg)
{
    int64_t next_conversion_us = 0, read_due_us = 0;
    if (s_onewire_bus_count) {
        request_search(true);
    }
    for (;;) {
        if (__atomic_exchange_n(&s_search_pending, false, __ATOMIC_ACQUIRE)) {
            search_sensors(__atomic_exchange_n(&s_search_replace, false, __ATOMIC_RELAXED));
            read_due_us = 0;
            next_conversion_us = 0; // read the new list right away
        }
        if (s_analog_input_count) {
            sample_analog_inputs();
        }
        int64_t now_us = esp_timer_get_time();
        if (s_onewire_bus_count && read_due_us && now_us >= read_due_us) {
            read_sensors();
            read_due_us = 0;
        }
        if (s_onewire_bus_count && !read_due_us && now_us >= next_conversion_us) {
            start_conversion();
            read_due_us = now_us + CONVERSION_MS * 1000LL;
            next_conversion_us = now_us + ONEWIRE_PERIOD_MS * 1000LL;
        }
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

/** @brief Average OVERSAMPLE_COUNT readings per input and store them as terminal value in 1/1000. */
static void sample_analog_inputs(void)
{
    for (int i = 0; i < s_analog_input_count; i++) {
        const analog_input_t *input = &s_analog_inputs[i];
        int raw_sum = 0, good_samples = 0;
        for (int sample_index = 0; sample_index < OVERSAMPLE_COUNT; sample_index++) {
            int raw_value;
            if (adc_oneshot_read(s_adc_unit, input->channel, &raw_value) == ESP_OK) {
                raw_sum += raw_value;
                good_samples++;
            }
        }
        if (!good_samples) {
            continue;
        }
        float milli_value = (float)analog_millivolts(input, raw_sum / good_samples) * input->factor;
        localio_sensors_set_input(i, (uint16_t)(milli_value <= 0 ? 0 : milli_value >= 65535 ? 65535 : milli_value + 0.5f));
    }
}

/** @brief Pin voltage [mV] of an averaged raw reading. */
static int analog_millivolts(const analog_input_t *input, int raw_average)
{
    int millivolts;
    if (input->calibration && adc_cali_raw_to_voltage(input->calibration, raw_average, &millivolts) == ESP_OK) {
        return millivolts;
    }
    return raw_average * 3100 / 4095; // 12 dB attenuation, uncalibrated
}

/** @brief Enumerate every bus and hand the supported sensors (all buses together) to localio_sensors. */
static void search_sensors(bool replace_list)
{
    uint64_t rom_codes[LOCALIO_SENSORS_MAX];
    int found_count = 0;
    if (replace_list) {
        s_sensor_location_count = 0;
    }
    for (int bus_index = 0; bus_index < s_onewire_bus_count; bus_index++) {
        onewire_device_iter_handle_t iterator;
        if (onewire_new_device_iter(s_onewire_buses[bus_index], &iterator) != ESP_OK) {
            continue;
        }
        onewire_device_t device;
        while (found_count < LOCALIO_SENSORS_MAX && onewire_device_iter_get_next(iterator, &device) == ESP_OK) {
            uint8_t family = (uint8_t)device.address;
            if (family != LOCALIO_FAMILY_DS18B20 && family != LOCALIO_FAMILY_DS18S20) {
                ESP_LOGW(TAG, "1-Wire device %016llx: family 0x%02x not supported, ignored",
                         (unsigned long long)device.address, family);
                continue;
            }
            rom_codes[found_count++] = device.address;
            remember_location(device.address, bus_index);
        }
        onewire_del_device_iter(iterator);
    }
    localio_sensors_search_done(rom_codes, found_count, replace_list);
    ESP_LOGI(TAG, "1-Wire search: %d sensor(s) on %d bus(es)", found_count, s_onewire_bus_count);
}

/** @brief Record (or move) a sensor's bus; when the table is full the sensor stays unknown (read fails). */
static void remember_location(uint64_t rom_code, int bus_index)
{
    for (int i = 0; i < s_sensor_location_count; i++) {
        if (s_sensor_locations[i].rom_code == rom_code) {
            s_sensor_locations[i].bus_index = bus_index;
            return;
        }
    }
    if (s_sensor_location_count < (int)(sizeof s_sensor_locations / sizeof s_sensor_locations[0])) {
        s_sensor_locations[s_sensor_location_count++] = (sensor_location_t){.rom_code = rom_code, .bus_index = bus_index};
    }
}

/** @brief CONVERT T on every sensor of every bus at once (SKIP ROM). */
static void start_conversion(void)
{
    const uint8_t command[] = {ONEWIRE_SKIP_ROM, ONEWIRE_CONVERT_T};
    for (int bus_index = 0; bus_index < s_onewire_bus_count; bus_index++) {
        if (onewire_bus_reset(s_onewire_buses[bus_index]) == ESP_OK) {
            onewire_bus_write_bytes(s_onewire_buses[bus_index], command, sizeof command);
        }
    }
}

/** @brief Read every listed sensor's scratchpad and store the temperature (or the failure). */
static void read_sensors(void)
{
    uint64_t rom_codes[LOCALIO_SENSORS_MAX];
    int sensor_count = localio_sensors_list(rom_codes, LOCALIO_SENSORS_MAX);
    for (int i = 0; i < sensor_count; i++) {
        uint8_t scratchpad[9];
        int16_t centi_celsius = 0;
        bool read_ok = read_scratchpad(rom_codes[i], scratchpad) &&
                       localio_ds18x20_temperature((uint8_t)rom_codes[i], scratchpad, &centi_celsius);
        localio_sensors_reading(rom_codes[i], read_ok, centi_celsius);
    }
}

static onewire_bus_handle_t sensor_bus(uint64_t rom_code);

/** @brief MATCH ROM + READ SCRATCHPAD of one sensor on its bus; false if nobody answered the reset. */
static bool read_scratchpad(uint64_t rom_code, uint8_t scratchpad[9])
{
    onewire_bus_handle_t bus = sensor_bus(rom_code);
    if (!bus) {
        return false;
    }
    uint8_t command[10] = {ONEWIRE_MATCH_ROM};
    for (int i = 0; i < 8; i++) {
        command[1 + i] = (uint8_t)(rom_code >> (8 * i)); // ROM goes out family code first
    }
    command[9] = ONEWIRE_READ_PAD;
    return onewire_bus_reset(bus) == ESP_OK && onewire_bus_write_bytes(bus, command, sizeof command) == ESP_OK &&
           onewire_bus_read_bytes(bus, scratchpad, 9) == ESP_OK;
}

/** @brief Bus a sensor was last found on, NULL if unknown. */
static onewire_bus_handle_t sensor_bus(uint64_t rom_code)
{
    for (int i = 0; i < s_sensor_location_count; i++) {
        if (s_sensor_locations[i].rom_code == rom_code) {
            return s_onewire_buses[s_sensor_locations[i].bus_index];
        }
    }
    return NULL;
}

#endif // CONFIG_CLU_LOCAL_IO
