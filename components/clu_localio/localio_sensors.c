/**
 * @file localio_sensors.c
 * @brief TF-Bus module side of the on-board analog inputs and 1-Wire sensors (see localio_sensors.h).
 *
 * s_modules[0] is the ANALOG_DIN module, s_modules[1 + i] the ONE_WIRE module of s_sensors[i].
 * Everything is under s_lock: the bus task answers frames, the measuring task stores values.
 */
#include "localio_sensors.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tfbus_frame.h"
#include "tfbus_vmod.h"

/** @brief One 1-Wire sensor. */
typedef struct {
    uint64_t rom_code;
    uint8_t state;          // TFBUS_ONEWIRE_*
    uint8_t failed_reads;   // in a row
    int16_t centi_celsius;  // last good reading
} sensor_t;

static SemaphoreHandle_t s_lock;
static localio_search_request_fn_t s_request_search;
static tfbus_vmod_t s_modules[1 + LOCALIO_SENSORS_MAX];
static sensor_t s_sensors[LOCALIO_SENSORS_MAX];
static int s_sensor_count;
static uint16_t s_inputs[TFBUS_ANALOG_INPUTS];
static uint8_t s_found_sensors;  // by the last search
static uint8_t s_searches_done;  // mod 256

static int analog_status(tfbus_vmod_t *module, uint8_t *status_buffer, size_t capacity);
static bool analog_command(tfbus_vmod_t *module, uint8_t type, const uint8_t *payload, size_t payload_length);
static int sensor_status(tfbus_vmod_t *module, uint8_t *status_buffer, size_t capacity);
static const tfbus_vmod_ops_t *sensor_ops(uint8_t family);
static sensor_t *find_sensor(uint64_t rom_code);

static const tfbus_vmod_ops_t s_analog_ops = {
    .hw_type = TFBUS_ANALOG_HW_TYPE,
    .fw_type = TFBUS_ANALOG_FW_TYPE,
    .fw_api_ver = TFBUS_ANALOG_FW_API_VER,
    .version = {TFBUS_ANALOG_FW_API_VER, 1, 0, 1},
    .status = analog_status,
    .command = analog_command,
};

// fwApiVer: the newest ONE_WIRE interface OM has for the family (module_one_wire_fv03_28.xml,
// module_one_wire_fv01_10.xml).
static const tfbus_vmod_ops_t s_ds18b20_ops = {
    .hw_type = TFBUS_ONEWIRE_HW_TYPE,
    .fw_type = LOCALIO_FAMILY_DS18B20,
    .fw_api_ver = 3,
    .version = {3, 1, 0, 1},
    .status = sensor_status,
};

static const tfbus_vmod_ops_t s_ds18s20_ops = {
    .hw_type = TFBUS_ONEWIRE_HW_TYPE,
    .fw_type = LOCALIO_FAMILY_DS18S20,
    .fw_api_ver = 1,
    .version = {1, 1, 0, 1},
    .status = sensor_status,
};

void localio_sensors_setup(uint32_t analog_serial_number, localio_search_request_fn_t request_search)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_request_search = request_search;
    s_modules[0] = (tfbus_vmod_t){.sn = analog_serial_number, .ops = &s_analog_ops};
    s_sensor_count = 0;
    memset(s_inputs, 0, sizeof s_inputs);
    s_found_sensors = 0;
    s_searches_done = 0;
}

void localio_sensors_set_input(int input_index, uint16_t milli_value)
{
    if (input_index < 0 || input_index >= TFBUS_ANALOG_INPUTS) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_inputs[input_index] = milli_value;
    xSemaphoreGive(s_lock);
}

void localio_sensors_search_done(const uint64_t *rom_codes, int found_count, bool replace_list)
{
    sensor_t sensors[LOCALIO_SENSORS_MAX];
    tfbus_vmod_t modules[LOCALIO_SENSORS_MAX];
    int sensor_count = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!replace_list) { // keep every known sensor, add the new ones after them
        memcpy(sensors, s_sensors, (size_t)s_sensor_count * sizeof *sensors);
        memcpy(modules, s_modules + 1, (size_t)s_sensor_count * sizeof *modules);
        sensor_count = s_sensor_count;
    }
    int supported_count = 0;
    for (int i = 0; i < found_count; i++) {
        uint64_t rom_code = rom_codes[i];
        const tfbus_vmod_ops_t *ops = sensor_ops((uint8_t)rom_code);
        supported_count += ops != NULL;
        if (sensor_count == LOCALIO_SENSORS_MAX) {
            continue;
        }
        bool listed = false;
        for (int j = 0; j < sensor_count && !listed; j++) {
            listed = sensors[j].rom_code == rom_code;
        }
        if (!ops || listed) {
            continue;
        }
        const sensor_t *known = find_sensor(rom_code);
        sensors[sensor_count] = known ? *known : (sensor_t){.rom_code = rom_code, .state = TFBUS_ONEWIRE_NOT_READ};
        modules[sensor_count] = known ? s_modules[1 + (known - s_sensors)]
                                      : (tfbus_vmod_t){.sn = localio_sensor_serial(rom_code), .ops = ops};
        sensor_count++;
    }
    for (int i = 0; i < sensor_count; i++) {
        s_sensors[i] = sensors[i];
        s_modules[1 + i] = modules[i];
        s_modules[1 + i].ctx = &s_sensors[i];
    }
    s_sensor_count = sensor_count;
    s_found_sensors = (uint8_t)(supported_count > 255 ? 255 : supported_count);
    s_searches_done++;
    xSemaphoreGive(s_lock);
}

int localio_sensors_list(uint64_t *rom_codes, int capacity)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int listed_count = 0;
    for (; listed_count < s_sensor_count && listed_count < capacity; listed_count++) {
        rom_codes[listed_count] = s_sensors[listed_count].rom_code;
    }
    xSemaphoreGive(s_lock);
    return listed_count;
}

void localio_sensors_reading(uint64_t rom_code, bool read_ok, int16_t centi_celsius)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sensor_t *sensor = find_sensor(rom_code);
    if (sensor && read_ok) {
        sensor->state = TFBUS_ONEWIRE_CONNECTED;
        sensor->failed_reads = 0;
        sensor->centi_celsius = centi_celsius;
    } else if (sensor && sensor->failed_reads < LOCALIO_SENSOR_MAX_FAILS && ++sensor->failed_reads == LOCALIO_SENSOR_MAX_FAILS) {
        sensor->state = TFBUS_ONEWIRE_DISCONNECTED;
    }
    xSemaphoreGive(s_lock);
}

int localio_sensors_frame(const uint8_t *request_frame, size_t request_length, uint8_t *reply_buffer,
                          size_t reply_capacity)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int reply_length = tfbus_vmod_frame(s_modules, 1 + s_sensor_count, request_frame, request_length, reply_buffer,
                                        reply_capacity);
    // Startup discovery / OM refresh: the master waits >= 1.2 s after its RESET before it
    // discovers, time enough to search the 1-Wire bus so new sensors answer DISCOVER.
    bool bus_reset = request_length >= 4 && request_frame[1] == TFBUS_BROADCAST && request_frame[2] == TFBUS_T_RESET;
    if (bus_reset && s_request_search) {
        s_request_search(true);
    }
    xSemaphoreGive(s_lock);
    return reply_length;
}

uint32_t localio_sensor_serial(uint64_t rom_code)
{
    uint32_t serial_number = (uint32_t)(rom_code >> 8) & 0x7FFFFFFF;
    return serial_number ? serial_number : 1;
}

uint8_t localio_onewire_crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < length; i++) {
        uint8_t input_byte = data[i];
        for (int bit = 0; bit < 8; bit++) {
            bool mix = (crc ^ input_byte) & 1;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            input_byte >>= 1;
        }
    }
    return crc;
}

bool localio_ds18x20_temperature(uint8_t family, const uint8_t scratchpad[9], int16_t *centi_celsius)
{
    bool all_zero = true;
    for (int i = 0; i < 9; i++) {
        all_zero = all_zero && scratchpad[i] == 0;
    }
    if (all_zero || localio_onewire_crc8(scratchpad, 8) != scratchpad[8]) {
        return false;
    }
    int16_t raw_temperature = (int16_t)(scratchpad[0] | scratchpad[1] << 8);
    if (family == LOCALIO_FAMILY_DS18B20) {
        // 1/16 °C; at 9-11 bit resolution (config byte 4, bits 5-6) the low bits are undefined.
        int resolution_bits = 9 + ((scratchpad[4] >> 5) & 3);
        int32_t sixteenths = raw_temperature & ~((1 << (12 - resolution_bits)) - 1);
        *centi_celsius = (int16_t)((sixteenths * 100 + (sixteenths >= 0 ? 8 : -8)) / 16);
        return true;
    }
    if (family == LOCALIO_FAMILY_DS18S20) {
        // 1/2 °C; extended resolution from COUNT_REMAIN (byte 6) and COUNT_PER_C (byte 7):
        // T = whole degrees - 0.25 + (COUNT_PER_C - COUNT_REMAIN) / COUNT_PER_C
        int count_remain = scratchpad[6], count_per_degree = scratchpad[7];
        if (count_per_degree == 0 || count_remain > count_per_degree) {
            *centi_celsius = (int16_t)(raw_temperature * 50);
            return true;
        }
        int32_t whole_degrees = raw_temperature >> 1; // arithmetic: rounds toward -inf like the sensor's truncation
        *centi_celsius = (int16_t)(whole_degrees * 100 - 25 + (count_per_degree - count_remain) * 100 / count_per_degree);
        return true;
    }
    return false;
}

/** @brief STATUS_POLL of the ANALOG_DIN module. */
static int analog_status(tfbus_vmod_t *module, uint8_t *status_buffer, size_t capacity)
{
    if (capacity < TFBUS_ANALOG_STATUS_LEN) {
        return -1;
    }
    tfbus_analog_status(s_inputs, s_found_sensors, s_searches_done, status_buffer);
    return TFBUS_ANALOG_STATUS_LEN;
}

/** @brief MODULE_CMD to the ANALOG_DIN module: a REFRESH record asks for a search; the rest is ACKed. */
static bool analog_command(tfbus_vmod_t *module, uint8_t type, const uint8_t *payload, size_t payload_length)
{
    if (type != TFBUS_T_MODULE_CMD || payload_length < 2) {
        return true;
    }
    tfbus_tlv_t records[8];
    // Payload: plen count {type len meta data}...; tfbus_parse_tlv() starts at the count.
    int record_count = tfbus_parse_tlv(payload + 1, payload_length - 1, records, 8);
    for (int i = 0; i < record_count && i < 8; i++) {
        if (records[i].type == TFBUS_ANALOG_REC_REFRESH && s_request_search) {
            s_request_search(false);
        }
    }
    return record_count >= 0;
}

/** @brief STATUS_POLL of a ONE_WIRE module. */
static int sensor_status(tfbus_vmod_t *module, uint8_t *status_buffer, size_t capacity)
{
    const sensor_t *sensor = module->ctx;
    if (capacity < TFBUS_ONEWIRE_STATUS_LEN) {
        return -1;
    }
    tfbus_onewire_status(sensor->state, sensor->centi_celsius, s_modules[0].sn, sensor->rom_code, status_buffer);
    return TFBUS_ONEWIRE_STATUS_LEN;
}

/** @brief Module type of a sensor family, NULL if OM has no interface for it. */
static const tfbus_vmod_ops_t *sensor_ops(uint8_t family)
{
    switch (family) {
    case LOCALIO_FAMILY_DS18B20: return &s_ds18b20_ops;
    case LOCALIO_FAMILY_DS18S20: return &s_ds18s20_ops;
    default: return NULL;
    }
}

/** @brief Listed sensor with @p rom_code, NULL if none. Lock held. */
static sensor_t *find_sensor(uint64_t rom_code)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].rom_code == rom_code) {
            return &s_sensors[i];
        }
    }
    return NULL;
}
