/**
 * @file ms_rs485.c
 * @brief MySensors RS485 transport frames and binary MyMessage (see ms_rs485.h).
 */
#include "ms_rs485.h"

#include <stdio.h>
#include <string.h>

#define SOH 0x01
#define STX 0x02
#define ETX 0x03
#define EOT 0x04
#define SYS_PACK 0x58

#define PROTOCOL_VERSION 2
#define MAX_PAYLOAD 25

void ms_rs485_rx_init(ms_rs485_rx_t *rx, uint8_t self)
{
    memset(rx, 0, sizeof *rx);
    rx->self = self;
}

int ms_rs485_rx_push(ms_rs485_rx_t *rx, uint8_t received_byte, ms_rs485_frame_t *out)
{
    switch (rx->phase) {
    case 0: // header window: SOH to from cmd len STX
        memmove(rx->hdr, rx->hdr + 1, 5);
        rx->hdr[5] = received_byte;
        // to == from is skipped like MyTransportRS485 does, except a node without an id (255)
        // broadcasting: its I_FIND_PARENT_REQUEST / I_ID_REQUEST must reach the gateway
        if (rx->hdr[0] != SOH || rx->hdr[5] != STX ||
            (rx->hdr[1] == rx->hdr[2] && rx->hdr[1] != MS_BROADCAST)) {
            return 0;
        }
        rx->f.to = rx->hdr[1];
        rx->f.from = rx->hdr[2];
        rx->cmd = rx->hdr[3];
        rx->f.len = rx->hdr[4];
        rx->calc = (uint8_t)(rx->hdr[1] + rx->hdr[2] + rx->hdr[3] + rx->hdr[4]);
        memset(rx->hdr, 0, sizeof rx->hdr);
        if (rx->f.len >= MS_RS485_MAX_DATA || rx->f.from == rx->self ||
            (rx->f.to != rx->self && rx->f.to != MS_BROADCAST)) {
            return 0; // too long, ours, or for another node
        }
        rx->pos = 0;
        rx->phase = rx->f.len ? 1 : 2;
        return 0;
    case 1:
        rx->f.data[rx->pos++] = received_byte;
        rx->calc = (uint8_t)(rx->calc + received_byte);
        if (rx->pos == rx->f.len) {
            rx->phase = 2;
        }
        return 0;
    case 2:
        if (received_byte != ETX) {
            rx->phase = 0;
            return -1;
        }
        rx->phase = 3;
        return 0;
    case 3:
        rx->cs = received_byte;
        rx->phase = 4;
        return 0;
    default:
        rx->phase = 0;
        if (received_byte != EOT || rx->cs != rx->calc) {
            return -1;
        }
        if (rx->cmd != SYS_PACK) {
            return 0; // valid, but not a MySensors packet
        }
        *out = rx->f;
        return 1;
    }
}

size_t ms_rs485_encode(uint8_t to, uint8_t from, const uint8_t *data, size_t len, uint8_t *out, size_t cap)
{
    if (len > MS_RS485_MAX_DATA || len + 9 > cap) {
        return 0;
    }
    size_t encoded_length = 0;
    uint8_t frame_checksum = (uint8_t)(to + from + SYS_PACK + len);
    out[encoded_length++] = SOH;
    out[encoded_length++] = to;
    out[encoded_length++] = from;
    out[encoded_length++] = SYS_PACK;
    out[encoded_length++] = (uint8_t)len;
    out[encoded_length++] = STX;
    for (size_t i = 0; i < len; i++) {
        out[encoded_length++] = data[i];
        frame_checksum = (uint8_t)(frame_checksum + data[i]);
    }
    out[encoded_length++] = ETX;
    out[encoded_length++] = frame_checksum;
    out[encoded_length++] = EOT;
    return encoded_length;
}

static uint32_t le32(const uint8_t *little_endian_bytes)
{
    return little_endian_bytes[0] | little_endian_bytes[1] << 8 | little_endian_bytes[2] << 16 | (uint32_t)little_endian_bytes[3] << 24;
}

bool ms_bin_decode(const uint8_t *encoded_message, size_t len, ms_bin_t *decoded_message)
{
    if (len < MS_BIN_HEADER || (encoded_message[3] & 0x03) != PROTOCOL_VERSION) {
        return false;
    }
    memset(decoded_message, 0, sizeof *decoded_message);
    decoded_message->last = encoded_message[0];
    decoded_message->sender = encoded_message[1];
    decoded_message->destination = encoded_message[2];
    uint8_t plen = encoded_message[3] >> 3;
    if (plen > MAX_PAYLOAD) {
        plen = MAX_PAYLOAD;
    }
    if (MS_BIN_HEADER + (size_t)plen > len) {
        plen = (uint8_t)(len - MS_BIN_HEADER); // short frame: what is there
    }
    decoded_message->m.cmd = encoded_message[4] & 0x07;
    decoded_message->req_echo = (encoded_message[4] >> 3) & 1;
    decoded_message->is_echo = (encoded_message[4] >> 4) & 1;
    decoded_message->ptype = encoded_message[4] >> 5;
    decoded_message->m.type = encoded_message[5];
    decoded_message->m.child = encoded_message[6];
    decoded_message->m.node = decoded_message->sender;
    decoded_message->m.ack = decoded_message->is_echo;
    if (decoded_message->m.cmd > MS_C_STREAM) {
        return false;
    }
    const uint8_t *payload_bytes = encoded_message + MS_BIN_HEADER;
    char *payload_text = decoded_message->m.payload;
    size_t cap = sizeof decoded_message->m.payload;
    uint8_t pad[8] = {0}; // numeric types shorter than declared read as 0
    memcpy(pad, payload_bytes, plen < sizeof pad ? plen : sizeof pad);
    switch (decoded_message->ptype) {
    case MS_P_STRING: {
        size_t text_length = strnlen((const char *)payload_bytes, plen);
        for (size_t i = 0; i < text_length && i < cap - 1; i++) {
            payload_text[i] = (payload_bytes[i] == '\n' || payload_bytes[i] == '\r') ? ' ' : (char)payload_bytes[i]; // keep the text protocol line-safe
        }
        payload_text[text_length < cap ? text_length : cap - 1] = 0;
        break;
    }
    case MS_P_BYTE:
        snprintf(payload_text, cap, "%u", pad[0]);
        break;
    case MS_P_INT16:
        snprintf(payload_text, cap, "%d", (int16_t)(pad[0] | pad[1] << 8));
        break;
    case MS_P_UINT16:
        snprintf(payload_text, cap, "%u", (unsigned)(uint16_t)(pad[0] | pad[1] << 8));
        break;
    case MS_P_LONG32:
        snprintf(payload_text, cap, "%ld", (long)(int32_t)le32(pad));
        break;
    case MS_P_ULONG32:
        snprintf(payload_text, cap, "%lu", (unsigned long)le32(pad));
        break;
    case MS_P_FLOAT32: {
        uint32_t float_bits = le32(pad);
        float float_value;
        memcpy(&float_value, &float_bits, sizeof float_value);
        int prec = pad[4] > 8 ? 8 : pad[4];
        snprintf(payload_text, cap, "%2.*f", prec, (double)float_value); // dtostrf(f, 2, prec)
        break;
    }
    default: // P_CUSTOM: hex
        for (size_t i = 0; i < plen && 2 * i + 2 < cap; i++) {
            snprintf(payload_text + 2 * i, 3, "%02X", payload_bytes[i]);
        }
        break;
    }
    return true;
}

static int hexval(char character)
{
    return character >= '0' && character <= '9' ? character - '0' : character >= 'a' && character <= 'f' ? character - 'a' + 10 : character >= 'A' && character <= 'F' ? character - 'A' + 10 : 0;
}

static size_t header(uint8_t *out, uint8_t sender, uint8_t dest, uint8_t plen, uint8_t cmd, uint8_t req_echo,
                     uint8_t ptype, uint8_t type, uint8_t sensor)
{
    out[0] = sender; // last
    out[1] = sender;
    out[2] = dest;
    out[3] = (uint8_t)(PROTOCOL_VERSION | plen << 3);
    out[4] = (uint8_t)((cmd & 7) | (req_echo & 1) << 3 | ptype << 5);
    out[5] = type;
    out[6] = sensor;
    return MS_BIN_HEADER;
}

size_t ms_bin_encode(const ms_msg_t *message, uint8_t sender, uint8_t *out, size_t cap)
{
    size_t slen = strlen(message->payload);
    uint8_t buf[MAX_PAYLOAD];
    uint8_t plen, ptype;
    if (!slen) {
        buf[0] = 0; // the serial gateway sets a missing payload to (uint8_t)0
        plen = 1;
        ptype = MS_P_BYTE;
    } else if (message->cmd == MS_C_STREAM) {
        if (slen / 2 > MAX_PAYLOAD) {
            return 0;
        }
        plen = (uint8_t)(slen / 2);
        for (size_t i = 0; i < plen; i++) {
            buf[i] = (uint8_t)(hexval(message->payload[2 * i]) << 4 | hexval(message->payload[2 * i + 1]));
        }
        ptype = MS_P_CUSTOM;
    } else {
        if (slen > MAX_PAYLOAD) {
            return 0;
        }
        plen = (uint8_t)slen;
        memcpy(buf, message->payload, plen);
        ptype = MS_P_STRING;
    }
    if (MS_BIN_HEADER + (size_t)plen > cap) {
        return 0;
    }
    size_t header_length = header(out, sender, message->node, plen, message->cmd, message->ack ? 1 : 0, ptype, message->type, message->child);
    memcpy(out + header_length, buf, plen);
    return header_length + plen;
}

size_t ms_bin_encode_byte(uint8_t sender, uint8_t destination, uint8_t sensor, uint8_t cmd, uint8_t type,
                          uint8_t value, uint8_t *out, size_t cap)
{
    if (cap < MS_BIN_HEADER + 1) {
        return 0;
    }
    size_t header_length = header(out, sender, destination, 1, cmd, 0, MS_P_BYTE, type, sensor);
    out[header_length++] = value;
    return header_length;
}
