/**
 * @file tfbus_vmod.c
 * @brief Module side of TF-Bus: emulated modules (see tfbus_vmod.h).
 */
#include "tfbus_vmod.h"

#include <string.h>

#include "tfbus_frame.h"

/** @brief Module -> CLU frame AA 00 len payload XOR (len = payload + 1); 0 if it does not fit. */
static int reply(uint8_t *rx, size_t cap, const uint8_t *p, size_t n)
{
    if (n + 4 > cap) {
        return 0;
    }
    rx[0] = TFBUS_START;
    rx[1] = TFBUS_CLU_ADDR;
    rx[2] = (uint8_t)(n + 1);
    if (n) {
        memcpy(rx + 3, p, n);
    }
    rx[3 + n] = tfbus_xor(rx, 3 + n);
    return (int)(4 + n);
}

static int broadcast(tfbus_vmod_t *m, int n, uint8_t type, const uint8_t *pl, size_t plen, uint8_t *rx,
                     size_t rxcap)
{
    switch (type) {
    case TFBUS_T_RESET:
        for (int i = 0; i < n; i++) {
            m[i].addr = 0;
        }
        return 0; // modules on the wire must hear it too
    case TFBUS_T_DISCOVER:
        for (int i = 0; i < n; i++) {
            if (!m[i].addr) {
                uint32_t sn = m[i].sn;
                const uint8_t p[7] = {(uint8_t)sn, (uint8_t)(sn >> 8), (uint8_t)(sn >> 16), (uint8_t)(sn >> 24),
                                      m[i].ops->hw_type, m[i].ops->fw_type, m[i].ops->fw_api_ver};
                return reply(rx, rxcap, p, sizeof p);
            }
        }
        return 0; // all addressed: the next DISCOVER is for the wire
    case TFBUS_T_ADDR_ASSIGN:
        if (plen >= 5) {
            uint32_t sn = pl[0] | pl[1] << 8 | pl[2] << 16 | (uint32_t)pl[3] << 24;
            for (int i = 0; i < n; i++) {
                if (m[i].sn == sn) {
                    m[i].addr = pl[4];
                    return reply(rx, rxcap, NULL, 0);
                }
            }
        }
        return 0;
    default:
        return 0; // SEEK etc.: for the wire
    }
}

int tfbus_vmod_frame(tfbus_vmod_t *m, int n, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxcap)
{
    if (!m || n <= 0 || txlen < 4 || tx[0] != TFBUS_START || tfbus_xor(tx, txlen) != 0) {
        return 0;
    }
    uint8_t dest = tx[1], type = tx[2];
    const uint8_t *pl = tx + 3;
    size_t plen = txlen - 4;
    if (dest == TFBUS_BROADCAST) {
        return broadcast(m, n, type, pl, plen, rx, rxcap);
    }
    tfbus_vmod_t *mod = NULL;
    for (int i = 0; i < n && !mod; i++) {
        if (m[i].addr && m[i].addr == dest) {
            mod = &m[i];
        }
    }
    if (!mod) {
        return 0;
    }
    switch (type) {
    case TFBUS_T_RESET:
        mod->addr = 0;
        return -1;
    case TFBUS_T_STATUS_POLL: {
        uint8_t p[TFBUS_MAX_PAYLOAD];
        int len = mod->ops->status(mod, p, sizeof p);
        return len < 0 ? -1 : reply(rx, rxcap, p, (size_t)len);
    }
    case TFBUS_T_VERSION_QUERY:
        return reply(rx, rxcap, mod->ops->version, sizeof mod->ops->version);
    default:
        if (mod->ops->command && !mod->ops->command(mod, type, pl, plen)) {
            return -1;
        }
        return reply(rx, rxcap, NULL, 0);
    }
}

void tfbus_din8_status(uint8_t inputs, uint8_t outputs, uint8_t p[TFBUS_DIN8_STATUS_LEN])
{
    const uint8_t s[TFBUS_DIN8_STATUS_LEN] = {2, 0x00, 1, 1, inputs, 0x01, 1, 1, outputs};
    memcpy(p, s, sizeof s);
}

bool tfbus_din8_cmd(const uint8_t *pl, size_t plen, uint8_t *out)
{
    tfbus_tlv_t r[8];
    // Payload: plen count {type len meta data}...; tfbus_parse_tlv() starts at the count.
    int n = plen >= 2 ? tfbus_parse_tlv(pl + 1, plen - 1, r, 8) : -1;
    if (n < 0) {
        return false;
    }
    for (int k = 0; k < n && k < 8; k++) {
        if ((r[k].type == 0x08 || r[k].type == 0x09) && r[k].len >= 1) {
            *out = r[k].type == 0x08 ? (uint8_t)(*out | r[k].data[0]) : (uint8_t)(*out & ~r[k].data[0]);
        }
    }
    return true;
}
