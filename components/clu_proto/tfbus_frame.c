/**
 * @file tfbus_frame.c
 * @brief TFBUS 2.0 frame building/parsing and the streaming RX parser (pure C).
 */
#include "tfbus_frame.h"

#include <string.h>

uint8_t tfbus_xor(const uint8_t *buf, size_t len)
{
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum ^= buf[i];
    }
    return checksum;
}

size_t tfbus_build(uint8_t *out, size_t cap, uint8_t dest, uint8_t type,
                   const uint8_t *payload, size_t plen)
{
    if (plen > TFBUS_MAX_PAYLOAD || cap < plen + 4) {
        return 0;
    }
    out[0] = TFBUS_START;
    out[1] = dest;
    out[2] = type;
    if (plen) {
        memcpy(out + 3, payload, plen);
    }
    out[3 + plen] = tfbus_xor(out, 3 + plen);
    return 4 + plen;
}

size_t tfbus_reset(uint8_t *out, size_t cap, uint8_t dest)
{
    return tfbus_build(out, cap, dest, TFBUS_T_RESET, NULL, 0);
}

size_t tfbus_seek(uint8_t *out, size_t cap)
{
    return tfbus_build(out, cap, TFBUS_BROADCAST, TFBUS_T_SEEK, NULL, 0);
}

size_t tfbus_discover(uint8_t *out, size_t cap)
{
    return tfbus_build(out, cap, TFBUS_BROADCAST, TFBUS_T_DISCOVER, NULL, 0);
}

bool tfbus_parse_discover(const uint8_t *discover_payload, size_t plen, tfbus_ident_t *id)
{
    if (plen < 7) {
        return false;
    }
    id->sn = (uint32_t)discover_payload[0] | (uint32_t)discover_payload[1] << 8 | (uint32_t)discover_payload[2] << 16 | (uint32_t)discover_payload[3] << 24;
    id->hw_type = discover_payload[4];
    id->fw_type = discover_payload[5];
    id->fw_api_ver = discover_payload[6];
    return id->sn != 0;
}

size_t tfbus_addr_assign(uint8_t *out, size_t cap, uint32_t serial, uint8_t channel)
{
    const uint8_t address_payload[5] = {
        (uint8_t)serial, (uint8_t)(serial >> 8), (uint8_t)(serial >> 16), (uint8_t)(serial >> 24),
        channel,
    };
    return tfbus_build(out, cap, TFBUS_BROADCAST, TFBUS_T_ADDR_ASSIGN, address_payload, sizeof address_payload);
}

size_t tfbus_status_poll(uint8_t *out, size_t cap, uint8_t dest)
{
    return tfbus_build(out, cap, dest, TFBUS_T_STATUS_POLL, NULL, 0);
}

size_t tfbus_version_query(uint8_t *out, size_t cap, uint8_t dest)
{
    return tfbus_build(out, cap, dest, TFBUS_T_VERSION_QUERY, NULL, 0);
}

size_t tfbus_addr_confirm(uint8_t *out, size_t cap, uint8_t dest)
{
    const uint8_t confirmation_payload[1] = {0x02};
    return tfbus_build(out, cap, dest, TFBUS_T_ADDR_CONFIRM, confirmation_payload, 1);
}

size_t tfbus_module_cmd(uint8_t *out, size_t cap, uint8_t dest,
                        const tfbus_tlv_t *recs, size_t nrecs)
{
    uint8_t command_payload[TFBUS_MAX_PAYLOAD];
    size_t off = 2;
    if (nrecs == 0 || nrecs > 255) {
        return 0;
    }
    for (size_t i = 0; i < nrecs; i++) {
        if (off + 3 + recs[i].len > sizeof command_payload) {
            return 0;
        }
        command_payload[off++] = recs[i].type;
        command_payload[off++] = recs[i].len;
        command_payload[off++] = recs[i].meta;
        if (recs[i].len) {
            memcpy(command_payload + off, recs[i].data, recs[i].len);
        }
        off += recs[i].len;
    }
    command_payload[0] = (uint8_t)off; // plen includes itself and count byte
    command_payload[1] = (uint8_t)nrecs;
    return tfbus_build(out, cap, dest, TFBUS_T_MODULE_CMD, command_payload, off);
}

size_t tfbus_cmd(uint8_t *out, size_t cap, uint8_t dest, uint8_t cmd,
                 const uint8_t *data, size_t dlen)
{
    uint8_t command_payload[22] = {0};
    command_payload[0] = cmd;
    if (dlen > 21) {
        dlen = 21;
    }
    if (dlen) {
        memcpy(command_payload + 1, data, dlen);
    }
    return tfbus_build(out, cap, dest, TFBUS_T_TFBUS_CMD, command_payload, sizeof command_payload);
}

bool tfbus_parse_response(const uint8_t *response_frame, size_t flen,
                          const uint8_t **payload, size_t *plen)
{
    if (flen < 4 || response_frame[0] != TFBUS_START || response_frame[1] != TFBUS_CLU_ADDR) {
        return false;
    }
    uint8_t response_length = response_frame[2];
    if (response_length > 0x1D) {
        return false;
    }
    size_t frame_len = (3u + response_length) < 4u ? 4u : 3u + response_length;
    if (flen < frame_len || tfbus_xor(response_frame, frame_len - 1) != response_frame[frame_len - 1]) {
        return false;
    }
    *payload = response_frame + 3;
    *plen = response_length ? response_length - 1u : 0u;
    return true;
}

int tfbus_parse_tlv(const uint8_t *tlv_payload, size_t plen, tfbus_tlv_t *recs, size_t max)
{
    if (plen == 0) {
        return 0;
    }
    uint8_t count = tlv_payload[0];
    size_t off = 1;
    int record_count = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (off + 3 > plen) {
            return -1;
        }
        tfbus_tlv_t record = {.type = tlv_payload[off], .len = tlv_payload[off + 1], .meta = tlv_payload[off + 2]};
        off += 3;
        if (off + record.len > plen) {
            return -1;
        }
        record.data = tlv_payload + off;
        off += record.len;
        if ((size_t)record_count < max) {
            recs[record_count] = record;
        }
        record_count++;
    }
    if (off != plen) {
        return -1;
    }
    return record_count < (int)max ? record_count : (int)max;
}

void tfbus_rx_init(tfbus_rx_t *rx)
{
    rx->len = 0;
}

/**
 * @brief Expected length of a CLU -> module frame from its type byte.
 *
 * @param b   Buffered bytes starting at TFBUS_START (at least 3).
 * @param len Number of buffered bytes.
 * @return Frame length, or -1 if unknown (caller falls back to the length byte).
 */
static int typed_len(const uint8_t *buffered_frame, size_t len)
{
    // Module -> CLU replies (dest 0x00) carry a length byte, not a type, at b[2]: reading it as a
    // type would e.g. wait 9 bytes for a 5-byte reply with length byte 0x02 (= ADDR_ASSIGN).
    if (buffered_frame[1] == TFBUS_CLU_ADDR) {
        return -1;
    }
    switch (buffered_frame[2]) {
    case 0x02: return 9;
    case 0x04: return 4;
    case 0x07: return 4;
    case 0x09: return 5;
    case 0x0A: return 4;
    case 0x0B: return 4;
    case 0x18: return 7;
    case 0x2A: return 4;
    case 0x31: return 26;
    case 0xF9: return 4;
    case 0xFB: return 25;
    case 0xFD: return 7;
    case 0xFE: return 7;
    case 0xF0: return len >= 4 ? 4 + buffered_frame[3] : -1;
    default:   return -1;
    }
}

/** @brief Remove the first @p n bytes from the RX buffer. */
static void drop(tfbus_rx_t *rx, size_t bytes_to_drop)
{
    memmove(rx->buf, rx->buf + bytes_to_drop, rx->len - bytes_to_drop);
    rx->len -= bytes_to_drop;
}

void tfbus_rx_push(tfbus_rx_t *rx, const uint8_t *data, size_t len,
                   void (*frame_callback)(const uint8_t *frame, size_t len, void *ctx), void *ctx)
{
    while (len) {
        size_t room = sizeof rx->buf - rx->len;
        size_t bytes_copied = len < room ? len : room;
        memcpy(rx->buf + rx->len, data, bytes_copied);
        rx->len += bytes_copied;
        data += bytes_copied;
        len -= bytes_copied;

        while (rx->len >= 4) {
            if (rx->buf[0] != TFBUS_START) {
                const uint8_t *next_frame_start = memchr(rx->buf + 1, TFBUS_START, rx->len - 1);
                drop(rx, next_frame_start ? (size_t)(next_frame_start - rx->buf) : rx->len);
                continue;
            }
            int cand[2];
            int candidate_count = 0;
            int typed_length = typed_len(rx->buf, rx->len);
            int raw_length = 3 + rx->buf[2] < 4 ? 4 : 3 + rx->buf[2];
            if (typed_length > 0) {
                cand[candidate_count++] = typed_length;
            }
            if (raw_length != typed_length) {
                cand[candidate_count++] = raw_length;
            }
            bool delivered = false;
            bool need_more = false;
            for (int i = 0; i < candidate_count; i++) {
                size_t candidate_frame_length = (size_t)cand[i];
                if (candidate_frame_length > TFBUS_MAX_FRAME) {
                    continue;
                }
                if (rx->len < candidate_frame_length) {
                    need_more = true;
                    break;
                }
                if (tfbus_xor(rx->buf, candidate_frame_length - 1) == rx->buf[candidate_frame_length - 1]) {
                    frame_callback(rx->buf, candidate_frame_length, ctx);
                    drop(rx, candidate_frame_length);
                    delivered = true;
                    break;
                }
            }
            if (need_more) {
                break;
            }
            if (!delivered) {
                drop(rx, 1);
            }
        }
        if (rx->len == sizeof rx->buf) {
            drop(rx, 1); // garbage filled buffer: resync
        }
    }
}
