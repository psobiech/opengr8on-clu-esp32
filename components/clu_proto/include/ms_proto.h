/**
 * @file ms_proto.h
 * @brief MySensors serial protocol: one message per line.
 *
 * `<node-id>;<child-sensor-id>;<command>;<ack>;<type>;<payload>\n`, all fields but the payload
 * decimal 0..255. The payload is the rest of the line (V_POSITION carries ';' in it). Radio payloads
 * are at most 25 bytes; the gateway's own log lines (I_LOG_MESSAGE) can be longer and are cut to
 * MS_PAYLOAD_CAP - 1.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Longest payload a node accepts (radio frame limit). */
#define MS_MAX_PAYLOAD 25
/** @brief Payload buffer: longer payloads (gateway log lines) are cut. */
#define MS_PAYLOAD_CAP 64
/** @brief Longest serial line handled (longer lines are dropped). */
#define MS_LINE_CAP 128

/** @brief Gateway node id. */
#define MS_GATEWAY 0
/** @brief Broadcast / "no id yet" node id; also the child id of the node itself. */
#define MS_BROADCAST 255

/** @brief Command (message type). */
enum {
    MS_C_PRESENTATION = 0,
    MS_C_SET = 1,
    MS_C_REQ = 2,
    MS_C_INTERNAL = 3,
    MS_C_STREAM = 4,
};

/** @brief Internal message types (C_INTERNAL sub-type) the controller handles. */
enum {
    MS_I_BATTERY_LEVEL = 0,
    MS_I_TIME = 1,
    MS_I_VERSION = 2,
    MS_I_ID_REQUEST = 3,
    MS_I_ID_RESPONSE = 4,
    MS_I_CONFIG = 6,
    MS_I_FIND_PARENT_REQUEST = 7,
    MS_I_FIND_PARENT_RESPONSE = 8,
    MS_I_LOG_MESSAGE = 9,
    MS_I_SKETCH_NAME = 11,
    MS_I_SKETCH_VERSION = 12,
    MS_I_GATEWAY_READY = 14,
    MS_I_PRESENTATION = 19,
    MS_I_HEARTBEAT_RESPONSE = 22,
    MS_I_PING = 24,
    MS_I_PONG = 25,
    MS_I_REGISTRATION_REQUEST = 26,
    MS_I_REGISTRATION_RESPONSE = 27,
    MS_I_SIGNAL_REPORT_REVERSE = 30,
};

/** @brief One message. */
typedef struct {
    uint8_t node;   /**< Node id (0 = gateway, 255 = broadcast / unassigned). */
    uint8_t child;  /**< Child sensor id (255 = the node itself). */
    uint8_t cmd;    /**< Command, MS_C_*. */
    uint8_t ack;    /**< Outgoing: request an ack; incoming: this is the ack. */
    uint8_t type;   /**< Sub-type: S_* / V_* / I_* / ST_* depending on @ref cmd. */
    char payload[MS_PAYLOAD_CAP]; /**< NUL-terminated. */
} ms_msg_t;

/**
 * @brief Parse one line (without the '\n'; a trailing '\r' is ignored).
 * @param line Line.
 * @param out  Parsed message.
 * @return false if the line is not a MySensors message (bad field count / number, cmd > 4,
 *         ack > 1).
 */
bool ms_parse(const char *line, ms_msg_t *out);

/**
 * @brief Format a message as a line, '\n' included.
 * @param m   Message.
 * @param out Buffer.
 * @param cap Capacity of @p out.
 * @return Line length, or -1 if it does not fit / the payload has a line break.
 */
int ms_format(const ms_msg_t *m, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
