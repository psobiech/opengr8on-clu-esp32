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

int ms_rs485_rx_push(ms_rs485_rx_t *rx, uint8_t c, ms_rs485_frame_t *out)
{
    switch (rx->phase) {
    case 0: // header window: SOH to from cmd len STX
        memmove(rx->hdr, rx->hdr + 1, 5);
        rx->hdr[5] = c;
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
        rx->f.data[rx->pos++] = c;
        rx->calc = (uint8_t)(rx->calc + c);
        if (rx->pos == rx->f.len) {
            rx->phase = 2;
        }
        return 0;
    case 2:
        if (c != ETX) {
            rx->phase = 0;
            return -1;
        }
        rx->phase = 3;
        return 0;
    case 3:
        rx->cs = c;
        rx->phase = 4;
        return 0;
    default:
        rx->phase = 0;
        if (c != EOT || rx->cs != rx->calc) {
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
    size_t n = 0;
    uint8_t cs = (uint8_t)(to + from + SYS_PACK + len);
    out[n++] = SOH;
    out[n++] = to;
    out[n++] = from;
    out[n++] = SYS_PACK;
    out[n++] = (uint8_t)len;
    out[n++] = STX;
    for (size_t i = 0; i < len; i++) {
        out[n++] = data[i];
        cs = (uint8_t)(cs + data[i]);
    }
    out[n++] = ETX;
    out[n++] = cs;
    out[n++] = EOT;
    return n;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

bool ms_bin_decode(const uint8_t *b, size_t len, ms_bin_t *o)
{
    if (len < MS_BIN_HEADER || (b[3] & 0x03) != PROTOCOL_VERSION) {
        return false;
    }
    memset(o, 0, sizeof *o);
    o->last = b[0];
    o->sender = b[1];
    o->destination = b[2];
    uint8_t plen = b[3] >> 3;
    if (plen > MAX_PAYLOAD) {
        plen = MAX_PAYLOAD;
    }
    if (MS_BIN_HEADER + (size_t)plen > len) {
        plen = (uint8_t)(len - MS_BIN_HEADER); // short frame: what is there
    }
    o->m.cmd = b[4] & 0x07;
    o->req_echo = (b[4] >> 3) & 1;
    o->is_echo = (b[4] >> 4) & 1;
    o->ptype = b[4] >> 5;
    o->m.type = b[5];
    o->m.child = b[6];
    o->m.node = o->sender;
    o->m.ack = o->is_echo;
    if (o->m.cmd > MS_C_STREAM) {
        return false;
    }
    const uint8_t *p = b + MS_BIN_HEADER;
    char *s = o->m.payload;
    size_t cap = sizeof o->m.payload;
    uint8_t pad[8] = {0}; // numeric types shorter than declared read as 0
    memcpy(pad, p, plen < sizeof pad ? plen : sizeof pad);
    switch (o->ptype) {
    case MS_P_STRING: {
        size_t n = strnlen((const char *)p, plen);
        for (size_t i = 0; i < n && i < cap - 1; i++) {
            s[i] = (p[i] == '\n' || p[i] == '\r') ? ' ' : (char)p[i]; // keep the text protocol line-safe
        }
        s[n < cap ? n : cap - 1] = 0;
        break;
    }
    case MS_P_BYTE:
        snprintf(s, cap, "%u", pad[0]);
        break;
    case MS_P_INT16:
        snprintf(s, cap, "%d", (int16_t)(pad[0] | pad[1] << 8));
        break;
    case MS_P_UINT16:
        snprintf(s, cap, "%u", (unsigned)(uint16_t)(pad[0] | pad[1] << 8));
        break;
    case MS_P_LONG32:
        snprintf(s, cap, "%ld", (long)(int32_t)le32(pad));
        break;
    case MS_P_ULONG32:
        snprintf(s, cap, "%lu", (unsigned long)le32(pad));
        break;
    case MS_P_FLOAT32: {
        uint32_t u = le32(pad);
        float f;
        memcpy(&f, &u, sizeof f);
        int prec = pad[4] > 8 ? 8 : pad[4];
        snprintf(s, cap, "%2.*f", prec, (double)f); // dtostrf(f, 2, prec)
        break;
    }
    default: // P_CUSTOM: hex
        for (size_t i = 0; i < plen && 2 * i + 2 < cap; i++) {
            snprintf(s + 2 * i, 3, "%02X", p[i]);
        }
        break;
    }
    return true;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
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

size_t ms_bin_encode(const ms_msg_t *m, uint8_t sender, uint8_t *out, size_t cap)
{
    size_t slen = strlen(m->payload);
    uint8_t buf[MAX_PAYLOAD];
    uint8_t plen, ptype;
    if (!slen) {
        buf[0] = 0; // the serial gateway sets a missing payload to (uint8_t)0
        plen = 1;
        ptype = MS_P_BYTE;
    } else if (m->cmd == MS_C_STREAM) {
        if (slen / 2 > MAX_PAYLOAD) {
            return 0;
        }
        plen = (uint8_t)(slen / 2);
        for (size_t i = 0; i < plen; i++) {
            buf[i] = (uint8_t)(hexval(m->payload[2 * i]) << 4 | hexval(m->payload[2 * i + 1]));
        }
        ptype = MS_P_CUSTOM;
    } else {
        if (slen > MAX_PAYLOAD) {
            return 0;
        }
        plen = (uint8_t)slen;
        memcpy(buf, m->payload, plen);
        ptype = MS_P_STRING;
    }
    if (MS_BIN_HEADER + (size_t)plen > cap) {
        return 0;
    }
    size_t n = header(out, sender, m->node, plen, m->cmd, m->ack ? 1 : 0, ptype, m->type, m->child);
    memcpy(out + n, buf, plen);
    return n + plen;
}

size_t ms_bin_encode_byte(uint8_t sender, uint8_t destination, uint8_t sensor, uint8_t cmd, uint8_t type,
                          uint8_t value, uint8_t *out, size_t cap)
{
    if (cap < MS_BIN_HEADER + 1) {
        return 0;
    }
    size_t n = header(out, sender, destination, 1, cmd, 0, MS_P_BYTE, type, sensor);
    out[n++] = value;
    return n;
}
