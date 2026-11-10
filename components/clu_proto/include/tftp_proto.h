/**
 * @file tftp_proto.h
 * @brief TFTP (RFC 1350) packet helpers — port of src/vclu/TFTPServer.ts. Pure C.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFTP_PORT       69    /**< Well-known TFTP port. */
#define TFTP_BLOCK_SIZE 512   /**< Data block size (no options negotiated). */

/** @brief TFTP opcodes. */
enum {
    TFTP_RRQ = 1,
    TFTP_WRQ = 2,
    TFTP_DATA = 3,
    TFTP_ACK = 4,
    TFTP_ERROR = 5,
};

/** @brief TFTP error codes. */
enum {
    TFTP_E_UNDEFINED = 0,
    TFTP_E_NOT_FOUND = 1,
    TFTP_E_ACCESS = 2,
    TFTP_E_DISK_FULL = 3,
    TFTP_E_ILLEGAL = 4,
    TFTP_E_UNKNOWN_TID = 5,
};

/**
 * @brief Parse an RRQ/WRQ packet: opcode, filename\\0, mode\\0.
 *
 * @param pkt      Packet.
 * @param len      Packet length.
 * @param opcode   Out: opcode.
 * @param filename Out: file name (points into @p pkt).
 * @param mode     Out: transfer mode (points into @p pkt).
 * @return false if malformed.
 */
bool tftp_parse_request(const uint8_t *pkt, size_t len, uint16_t *opcode,
                        const char **filename, const char **mode);

/**
 * @brief Map a CLU path to a VFS path.
 *
 * "a:\MAIN.LUA", "a:/x", "/a/x" or "MAIN.LUA" become "<root>/<drive>/<NAME>". Bare names default
 * to drive a; the drive letter is lowercased. Rejects "..", empty names and nested directories.
 *
 * @param root     VFS root directory.
 * @param filename CLU path from the request.
 * @param out      Output buffer.
 * @param cap      Capacity of @p out.
 * @return false if rejected.
 */
bool tftp_resolve_path(const char *root, const char *filename, char *out, size_t cap);

/**
 * @brief Build an ACK packet.
 * @param out   Output buffer (4 bytes).
 * @param block Block number.
 * @return Packet length.
 */
size_t tftp_ack(uint8_t *out, uint16_t block);

/**
 * @brief Build a DATA packet header.
 * @param out   Output buffer (4 bytes); data follows the header.
 * @param block Block number.
 * @return Header length.
 */
size_t tftp_data_hdr(uint8_t *out, uint16_t block);

/**
 * @brief Build an ERROR packet.
 * @param out  Output buffer.
 * @param cap  Capacity of @p out.
 * @param code TFTP_E_* error code.
 * @param msg  Error message.
 * @return Packet length.
 */
size_t tftp_error(uint8_t *out, size_t cap, uint16_t code, const char *msg);

/**
 * @brief Convert netascii to local text (CR LF -> LF, CR NUL -> CR).
 *
 * @param in         Input chunk.
 * @param len        Input length.
 * @param out        Output buffer (at least len bytes).
 * @param pending_cr Carries a pending CR across chunks.
 * @return Bytes written to @p out.
 */
size_t tftp_netascii_decode(const uint8_t *in, size_t len, uint8_t *out, bool *pending_cr);

#ifdef __cplusplus
}
#endif
