/**
 * @file ms_rs485.h
 * @brief MySensors RS485 transport (MY_RS485): frames on the bus and the binary MyMessage in them.
 *
 * Frame (MyTransportRS485, MySensors 2.x), one per message, 9600 8N1 by default:
 * @verbatim
 *   SOH  to  from  'X'(0x58)  len  STX  <len bytes: MyMessage>  ETX  cs  EOT
 *   cs = (to + from + 0x58 + len + data bytes) & 0xff
 * @endverbatim
 * The receiver slides a 6-byte window over the stream until it reads SOH ... STX with to != from
 * (the library's rule; to = from = 255, a node without an id broadcasting, is let through here),
 * then takes len bytes, ETX, the checksum and EOT; only frames for itself or the broadcast
 * address and not sent by itself are kept.
 *
 * MyMessage (protocol version 2), header 7 bytes then the payload (max 25 bytes):
 * @verbatim
 *   last  sender  destination  version_length  command_echo_payload  type  sensor  payload...
 *   version_length:       bits 0-1 version (2), bit 2 signed, bits 3-7 payload length
 *   command_echo_payload: bits 0-2 command, bit 3 echo request, bit 4 echo, bits 5-7 payload type
 * @endverbatim
 * Payload types are rendered as text like the library does for its serial gateway (getString) and
 * text from the controller becomes a P_STRING payload (no payload: P_BYTE 0, C_STREAM: hex -> bytes),
 * so the controller side (ms_msg_t) stays the serial protocol.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ms_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Max MyMessage bytes in a frame (MY_RS485_MAX_MESSAGE_LENGTH). */
#define MS_RS485_MAX_DATA 40
/** @brief Max frame length. */
#define MS_RS485_MAX_FRAME (6 + MS_RS485_MAX_DATA + 3)
/** @brief MyMessage header size. */
#define MS_BIN_HEADER 7

/** @brief Payload types (command_echo_payload bits 5-7). */
enum {
    MS_P_STRING = 0, MS_P_BYTE = 1, MS_P_INT16 = 2, MS_P_UINT16 = 3,
    MS_P_LONG32 = 4, MS_P_ULONG32 = 5, MS_P_CUSTOM = 6, MS_P_FLOAT32 = 7,
};

/** @brief A received frame. */
typedef struct {
    uint8_t to, from, len;
    uint8_t data[MS_RS485_MAX_DATA];
} ms_rs485_frame_t;

/** @brief Receive state machine. */
typedef struct {
    uint8_t self;                  /**< Own address (frames from it / for others are skipped). */
    uint8_t hdr[6];
    uint8_t phase, pos, cs, calc, cmd;
    ms_rs485_frame_t f;
} ms_rs485_rx_t;

/** @brief Reset the receiver for address @p self. */
void ms_rs485_rx_init(ms_rs485_rx_t *rx, uint8_t self);

/**
 * @brief Feed one byte.
 * @return 1: a valid frame for us is in @p out; -1: a frame for us was broken (no ETX / EOT, bad
 *         checksum); 0: nothing yet.
 */
int ms_rs485_rx_push(ms_rs485_rx_t *rx, uint8_t byte, ms_rs485_frame_t *out);

/**
 * @brief Build a frame.
 * @return Frame length, 0 if it does not fit / @p len is over MS_RS485_MAX_DATA.
 */
size_t ms_rs485_encode(uint8_t to, uint8_t from, const uint8_t *data, size_t len, uint8_t *out, size_t cap);

/** @brief A binary MyMessage with its routing fields. */
typedef struct {
    uint8_t last, sender, destination;
    uint8_t req_echo, is_echo, ptype;
    ms_msg_t m;                    /**< node = sender, ack = is_echo, payload as text. */
} ms_bin_t;

/**
 * @brief Decode a MyMessage.
 * @return false if too short, not protocol version 2 or the command is invalid.
 */
bool ms_bin_decode(const uint8_t *b, size_t len, ms_bin_t *out);

/**
 * @brief Encode a controller message (m->node = destination, m->ack = echo request) as sent by
 * @p sender: payload text as P_STRING, empty as P_BYTE 0, C_STREAM hex as P_CUSTOM.
 * @return Bytes written, 0 if it does not fit / the payload is too long.
 */
size_t ms_bin_encode(const ms_msg_t *m, uint8_t sender, uint8_t *out, size_t cap);

/**
 * @brief Encode a one-byte (P_BYTE) internal message from @p sender, e.g. I_PONG / I_FIND_PARENT_RESPONSE.
 * @return Bytes written.
 */
size_t ms_bin_encode_byte(uint8_t sender, uint8_t destination, uint8_t sensor, uint8_t cmd, uint8_t type,
                          uint8_t value, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
