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
 * A module type (IO_MODULE_DIN_8 now; dimmer, roller, LED RGBW later) supplies the identity,
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
 * @return Reply length in @p rx; 0 if not for these modules (or the reply does not fit); < 0 if
 *         handled without a reply.
 */
int tfbus_vmod_frame(tfbus_vmod_t *modules, int module_count, const uint8_t *tx, size_t txlen, uint8_t *rx,
                     size_t rxcap);

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

#ifdef __cplusplus
}
#endif
