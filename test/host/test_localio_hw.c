// Host tests for the on-board analog inputs / 1-Wire sensors hardware glue (clu_localio_sensors.c,
// included: state is static): the analog input list and ADC1 set-up, oversampling, calibration and
// scaling, the 1-Wire buses, the search across buses, CONVERT T / READ SCRATCHPAD on the right bus
// and the measuring task's timing, against a fake ADC, fake 1-Wire buses and a fake localio_sensors.
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static const char *s_analog_input_list = "";
static const char *s_onewire_gpio_list = "";
#define CONFIG_CLU_LOCAL_IO                1
#define CONFIG_CLU_LOCAL_IO_ANALOG_INPUTS  s_analog_input_list
#define CONFIG_CLU_LOCAL_IO_ONEWIRE_GPIOS  s_onewire_gpio_list

#include "../../components/clu_localio/clu_localio_sensors.c"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- fakes: state
#define FAKE_BUSES           16 // every start creates its buses anew
#define FAKE_DEVICES_PER_BUS 20
#define ROM_DS18B20_A        0x1100000000000a28ull
#define ROM_DS18B20_B        0x2200000000000b28ull
#define ROM_DS18S20          0x3300000000000c10ull
#define ROM_UNSUPPORTED      0x4400000000000d01ull
#define ROM_NOT_FOUND        0x5500000000000e28ull

static struct onewire_bus_t {
    int gpio;
    bool present;     // something answers the reset
    bool search_fails;
    uint64_t devices[FAKE_DEVICES_PER_BUS];
    int device_count;
    uint8_t last_write[16];
    int last_write_length;
    int writes;
    int reads;
} s_fake_buses[FAKE_BUSES];
static int s_fake_bus_count;
static int s_failing_bus_gpio = -1;
static onewire_bus_config_t s_last_bus_config;
static onewire_bus_rmt_config_t s_last_rmt_config;
static struct onewire_device_iter_t {
    struct onewire_bus_t *bus;
    int next_device;
} s_fake_iterator;
static int s_iterators_open;
static bool s_read_fails;

static struct adc_oneshot_unit_ctx_t { int unused; } s_fake_adc_unit;
static struct adc_cali_scheme_t { adc_channel_t channel; } s_fake_calibrations[TFBUS_ANALOG_INPUTS];
static bool s_adc_unit_fails;
static int s_adc_units_created;
static int s_raw_values[16];              // per channel
static adc_channel_t s_failing_channel = -1; // reads of this channel fail
static adc_channel_t s_uncalibrated_channel = -1;
static adc_atten_t s_last_channel_atten;

static uint32_t s_setup_serial;
static localio_search_request_fn_t s_setup_request_search;
static int s_setups;
static uint16_t s_input_values[TFBUS_ANALOG_INPUTS];
static int s_input_sets[TFBUS_ANALOG_INPUTS];
static uint64_t s_found_rom_codes[LOCALIO_SENSORS_MAX];
static int s_found_count = -1;
static bool s_found_replace_list;
static int s_searches;
static uint64_t s_listed_rom_codes[LOCALIO_SENSORS_MAX];
static int s_listed_count;
static struct {
    uint64_t rom_code;
    bool read_ok;
    int16_t centi_celsius;
} s_readings[LOCALIO_SENSORS_MAX];
static int s_reading_count;

static tfbus_uart_local_fn_t s_local_endpoint;
static void (*s_task_function)(void *);
static const char *s_task_name;
static uint32_t s_task_stack_size;
static UBaseType_t s_task_priority;
static bool s_task_create_fails;

static jmp_buf s_task_exit;
static const int64_t *s_clock_values; // esp_timer_get_time() per task iteration
static int s_clock_value_count, s_clock_reads;

static void test_analog_input_list(void);
static void test_sampling(void);
static void test_start(void);
static void test_task_cycle(void);
static void test_search(void);
static void run_task(const int64_t *clock_values, int clock_value_count);
static void reset_bus_traffic(void);
static int reading_index(uint64_t rom_code);

int main(void)
{
    test_analog_input_list();
    test_sampling();
    test_start();
    test_task_cycle();
    test_search();
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- analog input list
static void test_analog_input_list(void)
{
    check("empty list: no inputs, no ADC unit", setup_analog_inputs("") == 0 && s_adc_units_created == 0);
    check("blank list: no inputs", setup_analog_inputs("   ") == 0 && s_adc_units_created == 0);

    s_adc_unit_fails = true;
    check("ADC1 unit fails: no inputs", setup_analog_inputs("1") == 0);
    s_adc_unit_fails = false;

    check("not a number: list rejected", setup_analog_inputs("x") == 0);
    check("ADC2 pin: list rejected", setup_analog_inputs("1,15") == 0);
    check("pin without ADC channel: list rejected", setup_analog_inputs("99") == 0);
    check("more inputs than the module has: list rejected", setup_analog_inputs("1,2,3,4,5,6,7,8") == 0);
    check("as many inputs as the module has", setup_analog_inputs("1,2,3,4,5,6,7") == TFBUS_ANALOG_INPUTS);

    s_uncalibrated_channel = 1;
    check("list with factors, blanks and default factor", setup_analog_inputs(" 1:1.5, 2 ,3:6.667") == 3);
    check("input 0: GPIO1, channel 0, factor 1.5", s_analog_inputs[0].gpio == 1 && s_analog_inputs[0].channel == 0 &&
                                                       s_analog_inputs[0].factor == 1.5f);
    check("input 1: factor 1 when left out", s_analog_inputs[1].gpio == 2 && s_analog_inputs[1].factor == 1.0f);
    check("input 2: factor 6.667", s_analog_inputs[2].channel == 2 && s_analog_inputs[2].factor == 6.667f);
    check("calibrated inputs keep their scheme", s_analog_inputs[0].calibration && s_analog_inputs[2].calibration);
    check("input without calibration scheme: linear estimate", s_analog_inputs[1].calibration == NULL);
    check("channels at 12 dB attenuation", s_last_channel_atten == ADC_ATTEN_DB_12);
    s_uncalibrated_channel = -1;
}

// ---------------------------------------------------------------- oversampling, scaling
static void test_sampling(void)
{
    s_uncalibrated_channel = 1;
    s_analog_input_count = setup_analog_inputs("1:2,2,3");
    s_uncalibrated_channel = -1;
    s_raw_values[0] = 1000; // calibrated: the fake scheme gives 1 mV per step
    s_raw_values[1] = 4095; // uncalibrated: full scale = 3100 mV
    s_failing_channel = 2;
    s_input_sets[2] = 0;
    sample_analog_inputs();
    check("calibrated input: millivolts times factor", s_input_values[0] == 2000);
    check("uncalibrated input: linear estimate", s_input_values[1] == 3100);
    check("input whose reads all fail is left alone", s_input_sets[2] == 0);
    s_failing_channel = -1;

    s_analog_input_count = setup_analog_inputs("1:100,2:-1");
    s_raw_values[0] = 4000;
    s_raw_values[1] = 2000;
    sample_analog_inputs();
    check("value above 65535 clamped", s_input_values[0] == 65535);
    check("negative value clamped to 0", s_input_values[1] == 0);

    s_analog_input_count = setup_analog_inputs("1:0.5");
    s_raw_values[0] = 1001;
    sample_analog_inputs();
    check("value rounded to the nearest 1/1000", s_input_values[0] == 501);
}

// ---------------------------------------------------------------- start
static void test_start(void)
{
    s_analog_input_list = "";
    s_onewire_gpio_list = "";
    check("no inputs and no 1-Wire bus: not started", !clu_localio_sensors_start(1234) && s_setups == 0 &&
                                                          s_task_function == NULL);
    localio_channels_t channels;
    check("not started: no channels for any serial", !clu_localio_sensors_channels(1234, &channels));

    s_analog_input_list = "1";
    s_task_create_fails = true;
    check("analog inputs only: started", clu_localio_sensors_start(1234));
    check("module set up with the serial, no search requests", s_setups == 1 && s_setup_serial == 1234 &&
                                                                   s_setup_request_search == NULL);
    check("frames served by localio_sensors", s_local_endpoint == localio_sensors_frame);
    check("task failure is only logged", s_task_function == NULL);
    s_task_create_fails = false;

    s_analog_input_list = "1:2";
    s_onewire_gpio_list = "4, 5,6";
    s_failing_bus_gpio = 5;
    check("analog inputs and 1-Wire: started", clu_localio_sensors_start(5678));
    check("bus that fails is skipped", s_onewire_bus_count == 2 && s_onewire_buses[0]->gpio == 4 &&
                                           s_onewire_buses[1]->gpio == 6);
    check("1-Wire bus with pull-up, 10-byte receive", s_last_bus_config.flags.en_pull_up &&
                                                          s_last_rmt_config.max_rx_bytes == 10);
    check("1-Wire: search requests accepted", s_setup_request_search == request_search);
    check("sensor task: name, stack, priority", s_task_function == sensor_task &&
                                                    strcmp(s_task_name, "localio_sens") == 0 &&
                                                    s_task_stack_size == 4096 && s_task_priority == 3);
    check("channels: one analog input, one 1-Wire hub",
          clu_localio_sensors_channels(5678, &channels) && channels.analog_inputs == 0x01 &&
              channels.onewire_hubs == 1 && channels.digital_inputs == 0 && channels.digital_outputs == 0);
    check("channels: other serial -> not this module", !clu_localio_sensors_channels(1234, &channels));

    s_analog_input_list = "";
    s_failing_bus_gpio = -1;
    check("1-Wire only: started", clu_localio_sensors_start(42) && s_onewire_bus_count == 3 &&
                                      s_analog_input_count == 0);
    check("channels: 1-Wire only -> hub, no analog inputs",
          clu_localio_sensors_channels(42, &channels) && channels.analog_inputs == 0 && channels.onewire_hubs == 1);
}

// ---------------------------------------------------------------- task: search, conversion, reading
static void test_task_cycle(void)
{
    // Two buses: GPIO4 with a DS18B20 and an unsupported device, GPIO6 with a DS18S20.
    s_analog_input_list = "1:2";
    s_onewire_gpio_list = "4,6";
    clu_localio_sensors_start(5678);
    struct onewire_bus_t *first_bus = s_onewire_buses[0], *second_bus = s_onewire_buses[1];
    first_bus->devices[0] = ROM_DS18B20_A;
    first_bus->devices[1] = ROM_UNSUPPORTED;
    first_bus->device_count = 2;
    second_bus->devices[0] = ROM_DS18S20;
    second_bus->device_count = 1;
    s_raw_values[0] = 1234;
    s_input_sets[0] = 0;
    reset_bus_traffic();

    const int64_t first_cycle[] = {0, 500000, 900000};
    run_task(first_cycle, 3);
    check("first iteration: search on every bus, list replaced", s_searches == 1 && s_found_replace_list &&
                                                                     s_found_count == 2);
    check("supported sensors of both buses found, the rest ignored",
          s_found_rom_codes[0] == ROM_DS18B20_A && s_found_rom_codes[1] == ROM_DS18S20);
    check("search iterators closed", s_iterators_open == 0);
    // 3 iterations + the one that leaves the task (sampling comes before the clock read).
    check("analog inputs sampled every iteration", s_input_sets[0] == 4 && s_input_values[0] == 2468);

    // The conversion went out at 0 on both buses; the read was due at 800 ms (third iteration).
    s_listed_rom_codes[0] = ROM_DS18B20_A;
    s_listed_rom_codes[1] = ROM_DS18S20;
    s_listed_rom_codes[2] = ROM_NOT_FOUND;
    s_listed_count = 3;
    reset_bus_traffic();
    const int64_t conversion[] = {0};
    run_task(conversion, 1);
    const uint8_t convert_all[] = {0xcc, 0x44};
    check("CONVERT T with SKIP ROM on every bus", first_bus->writes == 1 && second_bus->writes == 1 &&
                                                      first_bus->last_write_length == 2 &&
                                                      memcmp(first_bus->last_write, convert_all, 2) == 0);

    reset_bus_traffic();
    const int64_t read_cycle[] = {0, 799999, 800000};
    run_task(read_cycle, 3);
    check("every listed sensor read once, at the conversion time", s_reading_count == 3);
    const uint8_t match_first[] = {0x55, 0x28, 0x0a, 0, 0, 0, 0, 0, 0x11, 0xbe};
    check("first sensor read on its bus: MATCH ROM family code first, READ SCRATCHPAD",
          first_bus->reads == 1 && first_bus->last_write_length == 10 &&
              memcmp(first_bus->last_write, match_first, 10) == 0);
    check("second sensor read on the other bus", second_bus->reads == 1 && second_bus->last_write[1] == 0x10);
    const int first_index = reading_index(ROM_DS18B20_A), second_index = reading_index(ROM_DS18S20);
    check("temperatures decoded from the scratchpads",
          first_index >= 0 && s_readings[first_index].read_ok && s_readings[first_index].centi_celsius == 0x0a * 10 &&
              second_index >= 0 && s_readings[second_index].read_ok &&
              s_readings[second_index].centi_celsius == 0x0c * 10);
    const int unknown_index = reading_index(ROM_NOT_FOUND);
    check("listed sensor never found: read fails", unknown_index >= 0 && !s_readings[unknown_index].read_ok);

    reset_bus_traffic();
    first_bus->present = false;
    s_read_fails = true;
    const int64_t failing_read[] = {0, 800000};
    run_task(failing_read, 2);
    check("nobody answers the reset: no conversion sent on that bus",
          first_bus->writes == 0 && second_bus->writes == 2);
    check("read fails on a silent bus and on a failed transfer",
          !s_readings[reading_index(ROM_DS18B20_A)].read_ok && !s_readings[reading_index(ROM_DS18S20)].read_ok);
    first_bus->present = true;
    s_read_fails = false;

    reset_bus_traffic();
    const int64_t next_period[] = {0, 800000, 1999999, 2000000};
    run_task(next_period, 4);
    check("next conversion every 2 s", first_bus->writes == 3); // convert, read, convert
}

// ---------------------------------------------------------------- search requests
static void test_search(void)
{
    struct onewire_bus_t *first_bus = s_onewire_buses[0], *second_bus = s_onewire_buses[1];

    // RefreshSensors: keep the list, add what is new; the task is woken at once.
    host_binary_given = 0;
    s_setup_request_search(false);
    check("request without replace: search flagged, task woken", s_search_pending && !s_search_replace &&
                                                                     host_binary_given);
    s_search_pending = false;

    // The sensor moved to the other bus.
    first_bus->device_count = 0;
    second_bus->devices[1] = ROM_DS18B20_A;
    second_bus->device_count = 2;
    s_searches = 0;
    search_sensors(false);
    check("search without replace: list kept", s_searches == 1 && !s_found_replace_list && s_found_count == 2);
    reset_bus_traffic();
    uint8_t scratchpad[9];
    check("moved sensor read on its new bus", read_scratchpad(ROM_DS18B20_A, scratchpad) &&
                                                  second_bus->reads == 1 && first_bus->reads == 0);

    first_bus->search_fails = true;
    for (int i = 0; i < FAKE_DEVICES_PER_BUS; i++) {
        second_bus->devices[i] = ROM_DS18B20_A + ((uint64_t)i << 8);
    }
    second_bus->device_count = FAKE_DEVICES_PER_BUS;
    s_setup_request_search(true);
    s_searches = 0;
    const int64_t replace_request[] = {0};
    run_task(replace_request, 1);
    check("bus whose search cannot start is skipped; found sensors capped",
          s_searches == 1 && s_found_replace_list && s_found_count == LOCALIO_SENSORS_MAX);
    first_bus->search_fails = false;

    const int location_capacity = (int)(sizeof s_sensor_locations / sizeof s_sensor_locations[0]);
    s_sensor_location_count = 0;
    for (int i = 0; i < location_capacity + 3; i++) {
        remember_location(0x1000 + (uint64_t)i, 0);
    }
    check("bus table full: further sensors stay unknown", s_sensor_location_count == location_capacity &&
                                                            sensor_bus(0x1000 + (uint64_t)location_capacity) == NULL);
    remember_location(0x1000, 1);
    check("known sensor moved to another bus", sensor_bus(0x1000) == s_onewire_buses[1]);
}

/** @brief Run the sensor task for one iteration per clock value (it is left when they run out). */
static void run_task(const int64_t *clock_values, int clock_value_count)
{
    s_clock_values = clock_values;
    s_clock_value_count = clock_value_count;
    s_clock_reads = 0;
    s_reading_count = 0;
    if (setjmp(s_task_exit) == 0) {
        sensor_task(NULL);
    }
}

static void reset_bus_traffic(void)
{
    for (int i = 0; i < FAKE_BUSES; i++) {
        s_fake_buses[i].writes = 0;
        s_fake_buses[i].reads = 0;
        s_fake_buses[i].last_write_length = 0;
    }
}

/** @brief Index of the last reading of @p rom_code in s_readings, -1 if none. */
static int reading_index(uint64_t rom_code)
{
    for (int i = s_reading_count - 1; i >= 0; i--) {
        if (s_readings[i].rom_code == rom_code) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------- fakes: ADC
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *init_config, adc_oneshot_unit_handle_t *ret_unit)
{
    if (s_adc_unit_fails || init_config->unit_id != ADC_UNIT_1) {
        return ESP_FAIL;
    }
    s_adc_units_created++;
    *ret_unit = &s_fake_adc_unit;
    return ESP_OK;
}

/** @brief GPIO1-10: ADC1 channel 0-9, GPIO11-20: ADC2. */
esp_err_t adc_oneshot_io_to_channel(int io_num, adc_unit_t *const unit_id, adc_channel_t *const channel)
{
    if (io_num < 1 || io_num > 20) {
        return ESP_ERR_INVALID_ARG;
    }
    *unit_id = io_num <= 10 ? ADC_UNIT_1 : ADC_UNIT_2;
    *channel = (io_num - 1) % 10;
    return ESP_OK;
}

esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t handle, adc_channel_t channel,
                                     const adc_oneshot_chan_cfg_t *config)
{
    s_last_channel_atten = config->atten;
    return ESP_OK;
}

esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t handle, adc_channel_t chan, int *out_raw)
{
    if (chan == s_failing_channel) {
        return ESP_FAIL;
    }
    *out_raw = s_raw_values[chan];
    return ESP_OK;
}

esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *config,
                                               adc_cali_handle_t *ret_handle)
{
    if (config->chan == s_uncalibrated_channel) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_fake_calibrations[config->chan % TFBUS_ANALOG_INPUTS].channel = config->chan;
    *ret_handle = &s_fake_calibrations[config->chan % TFBUS_ANALOG_INPUTS];
    return ESP_OK;
}

esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int *voltage)
{
    *voltage = raw;
    return ESP_OK;
}

// ---------------------------------------------------------------- fakes: 1-Wire
esp_err_t onewire_new_bus_rmt(const onewire_bus_config_t *bus_config, const onewire_bus_rmt_config_t *rmt_config,
                              onewire_bus_handle_t *ret_bus)
{
    s_last_bus_config = *bus_config;
    s_last_rmt_config = *rmt_config;
    if (bus_config->bus_gpio_num == s_failing_bus_gpio || s_fake_bus_count == FAKE_BUSES) {
        return ESP_FAIL;
    }
    struct onewire_bus_t *bus = &s_fake_buses[s_fake_bus_count++];
    memset(bus, 0, sizeof *bus);
    bus->gpio = bus_config->bus_gpio_num;
    bus->present = true;
    *ret_bus = bus;
    return ESP_OK;
}

esp_err_t onewire_bus_reset(onewire_bus_handle_t bus)
{
    return bus->present ? ESP_OK : ESP_FAIL;
}

esp_err_t onewire_bus_write_bytes(onewire_bus_handle_t bus, const uint8_t *tx_data, uint8_t tx_data_size)
{
    bus->writes++;
    bus->last_write_length = tx_data_size < sizeof bus->last_write ? tx_data_size : (int)sizeof bus->last_write;
    memcpy(bus->last_write, tx_data, (size_t)bus->last_write_length);
    return ESP_OK;
}

/** @brief Scratchpad of the sensor addressed by the last MATCH ROM: byte 0 = ROM byte 1. */
esp_err_t onewire_bus_read_bytes(onewire_bus_handle_t bus, uint8_t *rx_buf, size_t rx_buf_size)
{
    bus->reads++;
    if (s_read_fails) {
        return ESP_FAIL;
    }
    memset(rx_buf, 0, rx_buf_size);
    rx_buf[0] = bus->last_write[2];
    return ESP_OK;
}

esp_err_t onewire_new_device_iter(onewire_bus_handle_t bus, onewire_device_iter_handle_t *ret_iter)
{
    if (bus->search_fails) {
        return ESP_FAIL;
    }
    s_fake_iterator = (struct onewire_device_iter_t){.bus = bus};
    s_iterators_open++;
    *ret_iter = &s_fake_iterator;
    return ESP_OK;
}

esp_err_t onewire_device_iter_get_next(onewire_device_iter_handle_t iter, onewire_device_t *dev)
{
    if (iter->next_device >= iter->bus->device_count) {
        return ESP_ERR_NOT_SUPPORTED; // end of search
    }
    dev->bus = iter->bus;
    dev->address = iter->bus->devices[iter->next_device++];
    return ESP_OK;
}

esp_err_t onewire_del_device_iter(onewire_device_iter_handle_t iter)
{
    s_iterators_open--;
    return ESP_OK;
}

// ---------------------------------------------------------------- fakes: localio_sensors
void localio_sensors_setup(uint32_t analog_serial_number, localio_search_request_fn_t request_search_fn)
{
    s_setups++;
    s_setup_serial = analog_serial_number;
    s_setup_request_search = request_search_fn;
}

void localio_sensors_set_input(int input_index, uint16_t milli_value)
{
    s_input_values[input_index] = milli_value;
    s_input_sets[input_index]++;
}

void localio_sensors_search_done(const uint64_t *rom_codes, int found_count, bool replace_list)
{
    s_searches++;
    s_found_count = found_count;
    s_found_replace_list = replace_list;
    memcpy(s_found_rom_codes, rom_codes, (size_t)found_count * sizeof rom_codes[0]);
}

int localio_sensors_list(uint64_t *rom_codes, int capacity)
{
    memcpy(rom_codes, s_listed_rom_codes, (size_t)s_listed_count * sizeof rom_codes[0]);
    return s_listed_count;
}

void localio_sensors_reading(uint64_t rom_code, bool read_ok, int16_t centi_celsius)
{
    if (s_reading_count < LOCALIO_SENSORS_MAX) {
        s_readings[s_reading_count].rom_code = rom_code;
        s_readings[s_reading_count].read_ok = read_ok;
        s_readings[s_reading_count].centi_celsius = centi_celsius;
        s_reading_count++;
    }
}

/** @brief Fake decoding: 0.1 degree per unit of scratchpad byte 0. */
bool localio_ds18x20_temperature(uint8_t family, const uint8_t scratchpad[9], int16_t *centi_celsius)
{
    *centi_celsius = (int16_t)(scratchpad[0] * 10);
    return true;
}

int localio_sensors_frame(const uint8_t *request_frame, size_t request_length, uint8_t *reply_buffer,
                          size_t reply_capacity)
{
    return 0;
}

// ---------------------------------------------------------------- fakes: system
bool tfbus_uart_add_local(tfbus_uart_local_fn_t local_endpoint)
{
    s_local_endpoint = local_endpoint;
    return true;
}

BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *h)
{
    if (s_task_create_fails) {
        return pdFALSE;
    }
    s_task_function = fn;
    s_task_name = name;
    s_task_stack_size = stack;
    s_task_priority = prio;
    return pdPASS;
}

/** @brief Called once per task iteration: the next clock value, or leave the task when they ran out. */
int64_t esp_timer_get_time(void)
{
    if (s_clock_reads == s_clock_value_count) {
        longjmp(s_task_exit, 1);
    }
    return s_clock_values[s_clock_reads++];
}
