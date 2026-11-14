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
    /** Read all 16 inputs; false on failure. */
    bool (*read)(uint16_t *active, void *ctx);
    void *ctx;
} localio_hw_t;

/**
 * @brief Set the module serials and the hardware; all modules unaddressed, outputs off.
 * @param serial_numbers Serial numbers (non-zero, distinct), @p module_count of them.
 * @param module_count   Number of modules (1..LOCALIO_MAX_MODULES).
 * @param hardware       Hardware access; must stay valid.
 */
void localio_setup(const uint32_t *serial_numbers, int module_count, const localio_hw_t *hardware);

/**
 * @brief Handle one CLU -> module frame (::tfbus_uart_local_fn_t). Bus task only.
 * @return Reply length in @p rx, 0 if not for a local module, < 0 if handled without a reply.
 */
int localio_frame(const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxcap);

/**
 * @brief Board glue (Kconfig CLU_LOCAL_IO): serial from NVS / menuconfig, one PCF8575 on
 * I2C, local endpoint installed in tfbus_uart. Call after nvs_flash_init(), before the bus starts.
 */
void clu_localio_start(void);

#ifdef __cplusplus
}
#endif
