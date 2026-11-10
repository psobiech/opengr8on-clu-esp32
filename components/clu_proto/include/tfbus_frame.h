/**
 * @file tfbus_frame.h
 * @brief TFBUS 2.0 RS485 framing.
 *
 * Port of src/tfbus/TFBusFrame.ts, TFBusCommands.ts, transport/RxFrameParser.ts and
 * StatusPollResponse.ts. Pure C, no ESP-IDF deps.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFBUS_START        0xAA   /**< Frame start byte. */
#define TFBUS_BROADCAST    0xFF   /**< Broadcast destination address. */
#define TFBUS_CLU_ADDR     0x00   /**< Address of the CLU (module -> CLU frames). */
#define TFBUS_MIN_ADDR     1      /**< Lowest module address. */
#define TFBUS_MAX_ADDR     48     /**< Highest module address. */
/** @brief Max payload; same limit as the real CLU, both directions. */
#define TFBUS_MAX_PAYLOAD  28
/** @brief Max frame length; same limit as the real CLU, both directions. */
#define TFBUS_MAX_FRAME    32

/** @brief CLU -> module frame types. */
enum tfbus_frame_type {
    TFBUS_T_ADDR_ASSIGN   = 0x02,
    TFBUS_T_STATUS_POLL   = 0x04,
    TFBUS_T_RESET         = 0x07,
    TFBUS_T_DISCOVER      = 0x08, /**< Bcast; unaddressed module replies SN hwType fwType fwApiVer. */
    TFBUS_T_ADDR_CONFIRM  = 0x09,
    TFBUS_T_FORCE_STATUS  = 0x0B,
    TFBUS_T_LED_CMD       = 0x18,
    TFBUS_T_SEEK          = 0x2A,
    TFBUS_T_TFBUS_CMD     = 0x31,
    TFBUS_T_MODULE_CMD    = 0xF0,
    TFBUS_T_VERSION_QUERY = 0xF9,
    TFBUS_T_PROVISION     = 0xFB,
};

/** @brief One {type len meta data} record of a MODULE_CMD or STATUS_POLL payload. */
typedef struct {
    uint8_t type;
    uint8_t meta;
    uint8_t len;
    const uint8_t *data;   /**< Record data (len bytes); points into the source buffer. */
} tfbus_tlv_t;

/**
 * @brief XOR of all bytes (frame checksum).
 * @param buf Data.
 * @param len Length.
 * @return XOR checksum.
 */
uint8_t tfbus_xor(const uint8_t *buf, size_t len);

/**
 * @brief Build a CLU -> module frame: AA dest type payload XOR.
 *
 * @param out     Output buffer.
 * @param cap     Capacity of @p out.
 * @param dest    Destination address (or TFBUS_BROADCAST).
 * @param type    Frame type (::tfbus_frame_type).
 * @param payload Payload bytes (may be NULL if @p plen is 0).
 * @param plen    Payload length.
 * @return Frame length, 0 on error.
 */
size_t tfbus_build(uint8_t *out, size_t cap, uint8_t dest, uint8_t type,
                   const uint8_t *payload, size_t plen);

/**
 * @name Frame builders
 * Each returns the frame length written to @p out, 0 on error.
 * @{
 */
/** @brief RESET frame. */
size_t tfbus_reset(uint8_t *out, size_t cap, uint8_t dest);
/** @brief SEEK broadcast. */
size_t tfbus_seek(uint8_t *out, size_t cap);
/** @brief DISCOVER broadcast (answered by unaddressed modules). */
size_t tfbus_discover(uint8_t *out, size_t cap);
/** @brief ADDR_ASSIGN broadcast: give module @p serial the bus address @p channel. */
size_t tfbus_addr_assign(uint8_t *out, size_t cap, uint32_t serial, uint8_t channel);
/** @brief STATUS_POLL frame. */
size_t tfbus_status_poll(uint8_t *out, size_t cap, uint8_t dest);
/** @brief VERSION_QUERY frame. */
size_t tfbus_version_query(uint8_t *out, size_t cap, uint8_t dest);
/** @brief ADDR_CONFIRM frame. */
size_t tfbus_addr_confirm(uint8_t *out, size_t cap, uint8_t dest);
/** @brief MODULE_CMD (0xF0): plen count {type len meta data}... */
size_t tfbus_module_cmd(uint8_t *out, size_t cap, uint8_t dest,
                        const tfbus_tlv_t *recs, size_t nrecs);
/** @brief TFBUS_CMD (0x31): cmd + 21 data bytes, zero padded. */
size_t tfbus_cmd(uint8_t *out, size_t cap, uint8_t dest, uint8_t cmd,
                 const uint8_t *data, size_t dlen);
/** @} */

/**
 * @brief Parse a module -> CLU response: AA 00 len payload XOR, where len = payload_len + 1.
 *
 * @param frame   Complete frame.
 * @param flen    Frame length.
 * @param payload Out: payload (points into @p frame).
 * @param plen    Out: payload length.
 * @return true if valid.
 */
bool tfbus_parse_response(const uint8_t *frame, size_t flen,
                          const uint8_t **payload, size_t *plen);

/** @brief DISCOVER reply payload: SN(u32 LE) hwType fwType fwApiVer. */
typedef struct {
    uint32_t sn;
    uint8_t  hw_type, fw_type, fw_api_ver;
} tfbus_ident_t;

/**
 * @brief Parse a DISCOVER reply payload.
 * @param payload Payload.
 * @param plen    Payload length.
 * @param id      Out: module identity.
 * @return true if valid.
 */
bool tfbus_parse_discover(const uint8_t *payload, size_t plen, tfbus_ident_t *id);

/**
 * @brief Parse a STATUS_POLL payload: count {type len meta data}...
 *
 * @param payload Payload.
 * @param plen    Payload length.
 * @param recs    Out: records (data points into @p payload).
 * @param max     Capacity of @p recs.
 * @return Number of records parsed into @p recs (<= max), -1 on malformed payload.
 */
int tfbus_parse_tlv(const uint8_t *payload, size_t plen, tfbus_tlv_t *recs, size_t max);

/** @brief Streaming RX parser state (byte-resync, typed-length table + length-byte fallback). */
typedef struct {
    uint8_t buf[128];
    size_t  len;
} tfbus_rx_t;

/** @brief Reset the RX parser. */
void tfbus_rx_init(tfbus_rx_t *rx);

/**
 * @brief Push received bytes into the RX parser.
 *
 * @param rx   Parser state.
 * @param data Received bytes.
 * @param len  Number of bytes.
 * @param cb   Called as cb(frame, len, ctx) for each complete frame.
 * @param ctx  User context for @p cb.
 */
void tfbus_rx_push(tfbus_rx_t *rx, const uint8_t *data, size_t len,
                   void (*cb)(const uint8_t *frame, size_t len, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
