/**
 * @file localio_sensors.h
 * @brief On-board analog inputs and 1-Wire temperature sensors as TF-Bus modules.
 *
 * One ANALOG_DIN module (hwType 0x19: AnalogIN = the analog inputs, ONE_WIRE_HUB = the 1-Wire
 * bus) and one ONE_WIRE module (hwType 0xff, fwType = family code) per sensor found on the bus,
 * like the 1-Wire sensors of a real ANALOG_DIN. The firmware answers their TF-Bus frames itself
 * (wire format in tfbus_vmod.h), so discovery, addressing, CONFIG.JSON and OM treat them like
 * modules on the RS485 bus.
 *
 * The measuring task (clu_localio_sensors.c) stores inputs, search results and readings here; the
 * bus task answers the frames from them (localio_sensors_frame()). A bus RESET (startup discovery,
 * OM refresh) and the hub's RefreshSensors ask that task for a 1-Wire search. Thread-safe; pure C
 * (tfbus_vmod, FreeRTOS mutex): host-tested.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Max 1-Wire sensors (ONE_WIRE modules). */
#define LOCALIO_SENSORS_MAX 16

/** @brief 1-Wire family codes with a ONE_WIRE interface in OM. */
#define LOCALIO_FAMILY_DS18S20 0x10
#define LOCALIO_FAMILY_DS18B20 0x28

/** @brief Failed reads in a row after which a sensor counts as disconnected. */
#define LOCALIO_SENSOR_MAX_FAILS 3

/**
 * @brief Ask the measuring task for a 1-Wire search. Called with the sensor lock held: must not
 *        block or call back into this module.
 * @param replace_list true after a bus RESET: sensors not found any more are dropped (an OM
 *                     refresh then removes their modules); false (RefreshSensors): only adds.
 */
typedef void (*localio_search_request_fn_t)(bool replace_list);

/**
 * @brief Set up the ANALOG_DIN module; no sensors, inputs 0.
 * @param analog_serial_number Its serial number (non-zero); also the sensors' HubSerialNumber.
 * @param request_search       Search request hook (NULL = none).
 */
void localio_sensors_setup(uint32_t analog_serial_number, localio_search_request_fn_t request_search);

/**
 * @brief Store an analog input value.
 * @param input_index AnalogIN channel (0..TFBUS_ANALOG_INPUTS-1).
 * @param milli_value Value in 1/1000 (volts at the terminal, or mA for a current input).
 */
void localio_sensors_set_input(int input_index, uint16_t milli_value);

/**
 * @brief Store the result of a 1-Wire search.
 *
 * Unsupported families (not DS18S20 / DS18B20) are skipped, and sensors past LOCALIO_SENSORS_MAX.
 * The hub reports the number of supported sensors found.
 * Sensors kept from before keep their state and bus address; new ones start in the reset state.
 * @param rom_codes    ROM codes found (onewire_bus order: family code in the low byte).
 * @param found_count  Number of @p rom_codes.
 * @param replace_list Drop the sensors not found (see ::localio_search_request_fn_t).
 */
void localio_sensors_search_done(const uint64_t *rom_codes, int found_count, bool replace_list);

/**
 * @brief The sensors to read (current list).
 * @return Number written to @p rom_codes.
 */
int localio_sensors_list(uint64_t *rom_codes, int capacity);

/**
 * @brief Store a read attempt: a good read makes the sensor connected with @p centi_celsius;
 *        LOCALIO_SENSOR_MAX_FAILS failed ones in a row make it disconnected. Unknown ROMs ignored.
 */
void localio_sensors_reading(uint64_t rom_code, bool read_ok, int16_t centi_celsius);

/** @brief Handle one CLU -> module frame (::tfbus_uart_local_fn_t). Bus task only. */
int localio_sensors_frame(const uint8_t *request_frame, size_t request_length, uint8_t *reply_buffer,
                          size_t reply_capacity);

/** @brief ONE_WIRE module serial of a sensor: low 31 bits of its 48-bit serial (never 0). */
uint32_t localio_sensor_serial(uint64_t rom_code);

/** @brief Dallas / Maxim CRC8 (1-Wire ROM and scratchpad check). */
uint8_t localio_onewire_crc8(const uint8_t *data, size_t length);

/**
 * @brief Temperature from a DS18S20 / DS18B20 scratchpad (READ SCRATCHPAD, 9 bytes).
 * @param family        Family code.
 * @param scratchpad    The 9 bytes read.
 * @param centi_celsius Out: temperature [1/100 °C].
 * @return false on a CRC error, an all-zero read (bus shorted) or an unknown family.
 */
bool localio_ds18x20_temperature(uint8_t family, const uint8_t scratchpad[9], int16_t *centi_celsius);

#ifdef __cplusplus
}
#endif
