/**
 * @file tfbus_vmod.h
 * @brief Module side of TF-Bus: modules emulated by the firmware ("virtual modules").
 *
 * The bus master sends its frames to a local endpoint (tfbus_uart_add_local()) before the wire;
 * tfbus_vmod_frame() answers them for a set of emulated modules, so discovery, addressing,
 * polling, the drivers and CONFIG.JSON treat them like modules on the RS485 bus and OM adds them
 * to a project like real ones. The common part lives here:
 * @verbatim
 *   RESET          : forget the address (bcast: also goes on the wire)
 *   DISCOVER       : first unaddressed module -> SN(u32 LE) hwType fwType fwApiVer
 *   ADDR_ASSIGN    : own SN -> take the address, ACK
 *   VERSION_QUERY  : the type's 4 version bytes
 *   STATUS_POLL    : the type's status payload (or no reply: module offline)
 *   anything else  : the type's command hook, ACK
 * @endverbatim
 * A module type (IO_MODULE_DIN_8, ANALOG_DIN, ONE_WIRE) supplies the identity,
 * the status payload and the command handling (tfbus_vmod_ops_t). Pure C: host-tested.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tfbus_vmod tfbus_vmod_t;

/** @brief Emulated module type. */
typedef struct {
    uint8_t hw_type, fw_type, fw_api_ver;  /**< DISCOVER identity. */
    uint8_t version[4];                    /**< VERSION_QUERY reply: fwApiVer verA verB hwVer. */
    /** STATUS_POLL: write the status payload to @p status_buffer; return its length, < 0 for no reply
     *  (the master takes the module offline). */
    int (*status)(tfbus_vmod_t *module, uint8_t *status_buffer, size_t cap);
    /** Any other unicast frame (MODULE_CMD, class frames, ...); payload after the type byte.
     *  Return false for no reply. NULL = ACK everything. */
    bool (*command)(tfbus_vmod_t *module, uint8_t type, const uint8_t *payload, size_t plen);
} tfbus_vmod_ops_t;

/** @brief One emulated module. */
struct tfbus_vmod {
    uint32_t sn;                   /**< Serial number (non-zero, distinct). */
    uint8_t addr;                  /**< Bus address, 0 = unaddressed. */
    const tfbus_vmod_ops_t *ops;   /**< Type. */
    void *ctx;                     /**< Type state. */
};

/**
 * @brief Handle one CLU -> module frame for the modules @p modules[0..module_count) (::tfbus_uart_local_fn_t shape).
 * @return Reply length in @p reply_buffer; 0 if not for these modules (or the reply does not fit); < 0 if
 *         handled without a reply.
 */
int tfbus_vmod_frame(tfbus_vmod_t *modules, int module_count, const uint8_t *request_frame, size_t request_length, uint8_t *reply_buffer,
                     size_t reply_capacity);

/** @name IO_MODULE_DIN_8 (hwType 0x1e: 8 DIN + 8 DOUT) wire format
 * @{ */
#define TFBUS_DIN8_HW_TYPE    0x1e
#define TFBUS_DIN8_FW_TYPE    2
#define TFBUS_DIN8_FW_API_VER 2
/** @brief Status payload length. */
#define TFBUS_DIN8_STATUS_LEN 9

/** @brief STATUS_POLL payload: 02 | 00 01 01 [inputs] | 01 01 01 [outputs]. */
void tfbus_din8_status(uint8_t inputs, uint8_t outputs, uint8_t status_buffer[TFBUS_DIN8_STATUS_LEN]);

/**
 * @brief Apply a MODULE_CMD's ACTIVATE (08) / DEACTIVATE (09) [bitmask] records to @p out.
 * @return false if the payload does not parse.
 */
bool tfbus_din8_cmd(const uint8_t *payload, size_t plen, uint8_t *out);
/** @} */

/** @name ANALOG_DIN (hwType 0x19) and ONE_WIRE sensors (hwType 0xff) as emulated by the firmware
 * The real modules' wire format is not known. These records are this firmware's own, used only
 * between its emulated modules (on-board analog inputs and 1-Wire sensors) and its drivers:
 * @verbatim
 *   ANALOG_DIN status  : 02 | 20 0e 00 [7 x u16 LE input, 1/1000] | 21 02 00 [found, searches]
 *   ANALOG_DIN command : MODULE_CMD record 22 (no data): search the 1-Wire bus again
 *   ONE_WIRE status    : 01 | 23 0f 00 [state] [i16 LE 1/100 °C] [hub SN u32 LE] [ROM u64 LE]
 * @endverbatim
 * `found` = sensors found by the last search, `searches` = searches done (mod 256): a change
 * means a search finished. `state` is the ONEW_SENSOR Status: 0 disconnected, 1 connected
 * (temperature valid), 2 reset state (found, not read yet). A ONE_WIRE module's fwType is its
 * sensor's 1-Wire family code.
 * @{ */
#define TFBUS_ANALOG_HW_TYPE      0x19
#define TFBUS_ANALOG_FW_TYPE      2
#define TFBUS_ANALOG_FW_API_VER   2
#define TFBUS_ANALOG_INPUTS       7     /**< AnalogIN objects of the interface (fv02). */
#define TFBUS_ANALOG_REC_INPUTS   0x20  /**< Status record: input values. */
#define TFBUS_ANALOG_REC_HUB      0x21  /**< Status record: 1-Wire search results. */
#define TFBUS_ANALOG_REC_REFRESH  0x22  /**< MODULE_CMD record: search the 1-Wire bus. */
/** @brief ANALOG_DIN status payload length. */
#define TFBUS_ANALOG_STATUS_LEN   (1 + 3 + 2 * TFBUS_ANALOG_INPUTS + 3 + 2)

#define TFBUS_ONEWIRE_HW_TYPE     0xff
#define TFBUS_ONEWIRE_REC_SENSOR  0x23  /**< Status record: sensor reading. */
#define TFBUS_ONEWIRE_DISCONNECTED 0    /**< Sensor state: not answering. */
#define TFBUS_ONEWIRE_CONNECTED    1    /**< Sensor state: temperature valid. */
#define TFBUS_ONEWIRE_NOT_READ     2    /**< Sensor state: found, no temperature yet. */
/** @brief ONE_WIRE status payload length. */
#define TFBUS_ONEWIRE_STATUS_LEN  (1 + 3 + 1 + 2 + 4 + 8)

/** @brief ANALOG_DIN STATUS_POLL payload (see above). */
void tfbus_analog_status(const uint16_t input_values[TFBUS_ANALOG_INPUTS], uint8_t found_sensors,
                         uint8_t searches_done, uint8_t status_buffer[TFBUS_ANALOG_STATUS_LEN]);

/** @brief ONE_WIRE STATUS_POLL payload (see above). */
void tfbus_onewire_status(uint8_t sensor_state, int16_t centi_celsius, uint32_t hub_serial_number, uint64_t rom_code,
                          uint8_t status_buffer[TFBUS_ONEWIRE_STATUS_LEN]);
/** @} */

#ifdef __cplusplus
}
#endif
