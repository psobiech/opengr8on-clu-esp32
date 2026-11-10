/**
 * @file tftp_proto.c
 * @brief TFTP (RFC 1350) packet parsing/building and CLU path mapping (pure C).
 */
#include "tftp_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool tftp_parse_request(const uint8_t *pkt, size_t len, uint16_t *opcode,
                        const char **filename, const char **mode)
{
    if (len < 4) {
        return false;
    }
    *opcode = (uint16_t)(pkt[0] << 8 | pkt[1]);
    const uint8_t *fn = pkt + 2;
    const uint8_t *z1 = memchr(fn, 0, len - 2);
    if (!z1 || z1 == fn) {
        return false;
    }
    const uint8_t *md = z1 + 1;
    size_t rem = len - (size_t)(md - pkt);
    if (rem == 0) {
        return false;
    }
    const uint8_t *z2 = memchr(md, 0, rem);
    if (!z2) {
        return false;
    }
    *filename = (const char *)fn;
    *mode = (const char *)md;
    return true;
}

bool tftp_resolve_path(const char *root, const char *filename, char *out, size_t cap)
{
    char drive = 'a';
    const char *name = filename;

    if (isalpha((unsigned char)name[0]) && name[1] == ':' && (name[2] == '\\' || name[2] == '/')) {
        drive = (char)tolower((unsigned char)name[0]);
        name += 3;
    } else if ((name[0] == '/' || name[0] == '\\') && isalpha((unsigned char)name[1]) &&
               (name[2] == '/' || name[2] == '\\')) {
        drive = (char)tolower((unsigned char)name[1]);
        name += 3;
    }
    while (*name == '/' || *name == '\\') {
        name++;
    }
    if (!*name || strchr(name, '/') || strchr(name, '\\') || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        return false;
    }
    for (const char *c = name; *c; c++) {
        if ((unsigned char)*c < 0x20 || *c == ':') {
            return false;
        }
    }
    int n = snprintf(out, cap, "%s/%c/%s", root, drive, name);
    return n > 0 && (size_t)n < cap;
}

size_t tftp_ack(uint8_t *out, uint16_t block)
{
    out[0] = 0;
    out[1] = TFTP_ACK;
    out[2] = (uint8_t)(block >> 8);
    out[3] = (uint8_t)block;
    return 4;
}

size_t tftp_data_hdr(uint8_t *out, uint16_t block)
{
    out[0] = 0;
    out[1] = TFTP_DATA;
    out[2] = (uint8_t)(block >> 8);
    out[3] = (uint8_t)block;
    return 4;
}

size_t tftp_error(uint8_t *out, size_t cap, uint16_t code, const char *msg)
{
    size_t ml = strlen(msg);
    if (cap < 5) {
        return 0;
    }
    if (ml > cap - 5) {
        ml = cap - 5;
    }
    out[0] = 0;
    out[1] = TFTP_ERROR;
    out[2] = (uint8_t)(code >> 8);
    out[3] = (uint8_t)code;
    memcpy(out + 4, msg, ml);
    out[4 + ml] = 0;
    return 5 + ml;
}

size_t tftp_netascii_decode(const uint8_t *in, size_t len, uint8_t *out, bool *pending_cr)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = in[i];
        if (*pending_cr) {
            *pending_cr = false;
            if (c == '\n') {
                out[o++] = '\n';
                continue;
            }
            out[o++] = '\r';
            if (c == 0) {
                continue;
            }
        }
        if (c == '\r') {
            *pending_cr = true;
        } else {
            out[o++] = c;
        }
    }
    return o;
}
