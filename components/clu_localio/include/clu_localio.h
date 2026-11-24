/**
 * @file clu_localio.h
 * @brief On-board IO presented as TF-Bus modules (IO_MODULE_DIN_8: 8 DOUT + 8 DIN each).
 *
 * The firmware answers the TF-Bus frames for these modules itself (tfbus_uart local endpoint),
 * so the bus master, the IO_MODULE_DIN_8 driver, discovery and CONFIG.JSON treat them like
 * modules on the RS485 bus. Module 1 holds bits 0-7 of the 16-bit IO words, module 2 bits 8-15.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Max number of emulated modules. */
#define LOCALIO_MAX_MODULES 2

/** @brief Emulated module identity (DISCOVER / VERSION_QUERY), as a real IO_MODULE_DIN_8 reports. */
#define LOCALIO_HW_TYPE    0x1e
#define LOCALIO_FW_TYPE    2
#define LOCALIO_FW_API_VER 2

/** @brief Inputs are sampled at most this often [us]; a change counts after two equal samples. */
#define LOCALIO_SAMPLE_US  5000

/** @brief Hardware behind the modules (16-bit words, bit = channel, 1 = on / active). */
typedef struct {
    /** Drive all 16 outputs; false on failure. */
    bool (*write)(uint16_t on, void *ctx);
    /**
     * Read all 16 inputs and the actual level of all 16 outputs (on hardware that can read an
     * output back, e.g. a manual relay switch pulling the expander pin; otherwise as last
     * written); false on failure.
     */
    bool (*read)(uint16_t *active, uint16_t *outputs_on, void *ctx);
    void *ctx;
} localio_hw_t;

/**
 * @brief Channels of an on-board module that have hardware behind them (bit n = OM channel n).
 *
 * An emulated module shows OM all the objects of its interface; the other objects of these
 * modules (PowerSupplyVoltage, AnalogOUT) have no hardware at all.
 */
typedef struct {
    uint8_t digital_inputs;  /**< IO_MODULE_DIN_8 DIN (class 3) */
    uint8_t digital_outputs; /**< IO_MODULE_DIN_8 DOUT (class 4) */
    uint8_t analog_inputs;   /**< ANALOG_DIN AnalogIN (class 12) */
    uint8_t onewire_hubs;    /**< ANALOG_DIN ONE_WIRE_HUB (class 73) */
} localio_channels_t;

/**
 * @brief Set the module serials and the hardware; all modules unaddressed, outputs off.
 * @param serial_numbers Serial numbers (non-zero, distinct), @p module_count of them.
 * @param module_count   Number of modules (1..LOCALIO_MAX_MODULES).
 * @param hardware       Hardware access; must stay valid.
 */
void localio_setup(const uint32_t *serial_numbers, int module_count, const localio_hw_t *hardware);

/**
 * @brief Handle one CLU -> module frame (::tfbus_uart_local_fn_t). Bus task only.
 * @return Reply length in @p reply_buffer, 0 if not for a local module, < 0 if handled without a reply.
 */
int localio_frame(const uint8_t *request_frame, size_t request_length, uint8_t *reply_buffer, size_t reply_capacity);

/**
 * @brief Board glue (Kconfig CLU_LOCAL_IO): serials from NVS / menuconfig, one PCF8575 on
 * I2C (module 1), optional GPIO inputs (CLU_LOCAL_IO_GPIO_INPUTS, module 2), optional analog
 * inputs / 1-Wire sensors (clu_localio_sensors_start()), local endpoints installed in tfbus_uart.
 * Call after nvs_flash_init(), before the bus starts.
 */
void clu_localio_start(void);

/**
 * @brief Analog inputs (CLU_LOCAL_IO_ANALOG_INPUTS) and 1-Wire sensors (CLU_LOCAL_IO_ONEWIRE_GPIOS,
 * KinCony F8: CLU_LOCAL_IO_F8_DINx_MODE)
 * as an ANALOG_DIN module + ONE_WIRE modules (localio_sensors.h): ADC, 1-Wire bus, measuring task,
 * local endpoint. Called by clu_localio_start().
 * @param analog_serial_number ANALOG_DIN module serial.
 * @return false if neither is configured / both failed to start.
 */
bool clu_localio_sensors_start(uint32_t analog_serial_number);

/**
 * @brief Which channels of on-board module @p serial_number have hardware behind them (relays and
 * expander inputs; GPIO inputs except 1-Wire bus pins; configured analog inputs; the 1-Wire hub if
 * a bus is set up). The GPIO input module's DOUT and its DIN past the pin list have none.
 * @return false if @p serial_number is not an on-board IO_MODULE_DIN_8 / ANALOG_DIN module.
 */
bool clu_localio_channels(uint32_t serial_number, localio_channels_t *channels);

/** @brief clu_localio_channels() of the ANALOG_DIN module (clu_localio_sensors.c). */
bool clu_localio_sensors_channels(uint32_t serial_number, localio_channels_t *channels);

#ifdef __cplusplus
}
#endif
