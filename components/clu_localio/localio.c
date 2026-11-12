/**
 * @file localio.c
 * @brief TF-Bus module side of the on-board IO: emulated IO_MODULE_DIN_8 modules.
 *
 * Answers what a real IO_MODULE_DIN_8 answers (wire format in drv_digital_io.c):
 * @verbatim
 *   RESET          : forget the address (outputs keep their state; the driver re-sends them)
 *   DISCOVER       : unaddressed -> SN(u32 LE) hwType 0x1e fwType 2 fwApiVer 2
 *   ADDR_ASSIGN    : own SN -> take the address, ACK
 *   STATUS_POLL    : 02 | 00 01 01 [inputs] | 01 01 01 [outputs]
 *   VERSION_QUERY  : fwApiVer verA verB hwVer
 *   MODULE_CMD     : ACTIVATE (08) / DEACTIVATE (09) [bitmask] drive outputs, rest ACKed
 * @endverbatim
 * Frames for other addresses / serials return 0 and go on the RS485 wire. Pure C (tfbus_frame,
 * esp_timer only): host-tested.
 */
#include "clu_localio.h"

#include <string.h>

#include "esp_timer.h"
#include "tfbus_frame.h"

#define FAIL_RETRY_US 1000000 // after a failed input read, try the hardware again after 1 s

/** @brief VERSION_QUERY reply: fwApiVer, verA, verB, hwVer (fwVer "2.2.4", like a real module). */
static const uint8_t VERSION[4] = {LOCALIO_FW_API_VER, 2, 4, 3};

static struct {
    uint32_t sn;
    uint8_t addr; // 0 = unaddressed
} s_mod[LOCALIO_MAX_MODULES];
static int s_nmod;
static const localio_hw_t *s_hw;
static uint16_t s_out;      // outputs as last driven
static uint16_t s_in;       // debounced inputs
static uint16_t s_raw;      // last raw sample
static bool s_have_in;      // s_in / s_raw valid
static int64_t s_sampled;   // time of the last good sample
static int64_t s_failed;    // time of the last failed read (0 = none since the last good one)

void localio_setup(const uint32_t *sn, int n, const localio_hw_t *hw)
{
    s_nmod = n < 1 ? 1 : n > LOCALIO_MAX_MODULES ? LOCALIO_MAX_MODULES : n;
    for (int i = 0; i < s_nmod; i++) {
        s_mod[i].sn = sn[i];
        s_mod[i].addr = 0;
    }
    s_hw = hw;
    s_out = 0;
    s_have_in = false;
    s_failed = 0;
    s_hw->write(0, s_hw->ctx);
}

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

/**
 * @brief Refresh the debounced inputs (at most every LOCALIO_SAMPLE_US); false if unreadable.
 *
 * A new raw value is taken once two consecutive samples agree.
 */
static bool sample(void)
{
    int64_t now = esp_timer_get_time();
    if (s_have_in && now - s_sampled < LOCALIO_SAMPLE_US) {
        return true;
    }
    if (s_failed && now - s_failed < FAIL_RETRY_US) {
        return false; // do not stall every poll on a dead expander
    }
    uint16_t raw;
    if (!s_hw->read(&raw, s_hw->ctx)) {
        s_failed = now ? now : 1;
        s_have_in = false;
        return false;
    }
    s_failed = 0;
    if (!s_have_in || raw == s_raw) {
        s_in = raw;
    }
    s_raw = raw;
    s_sampled = now;
    s_have_in = true;
    return true;
}

/** @brief Apply ACTIVATE / DEACTIVATE records to module @p i's outputs; false on a write error. */
static bool module_cmd(int i, const uint8_t *pl, size_t plen)
{
    tfbus_tlv_t r[8];
    // Payload: plen count {type len meta data}...; tfbus_parse_tlv() starts at the count.
    int n = plen >= 2 ? tfbus_parse_tlv(pl + 1, plen - 1, r, 8) : -1;
    if (n < 0) {
        return false;
    }
    uint16_t out = s_out;
    for (int k = 0; k < n && k < 8; k++) {
        if ((r[k].type == 0x08 || r[k].type == 0x09) && r[k].len >= 1) {
            uint16_t mask = (uint16_t)(r[k].data[0] << (8 * i));
            out = r[k].type == 0x08 ? (out | mask) : (out & ~mask);
        }
    }
    if (out != s_out) {
        if (!s_hw->write(out, s_hw->ctx)) {
            return false;
        }
        s_out = out;
    }
    return true;
}

int localio_frame(const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxcap)
{
    if (!s_hw || txlen < 4 || tx[0] != TFBUS_START || tfbus_xor(tx, txlen) != 0) {
        return 0;
    }
    uint8_t dest = tx[1], type = tx[2];
    const uint8_t *pl = tx + 3;
    size_t plen = txlen - 4;

    if (dest == TFBUS_BROADCAST) {
        switch (type) {
        case TFBUS_T_RESET:
            for (int i = 0; i < s_nmod; i++) {
                s_mod[i].addr = 0;
            }
            return 0; // modules on the wire must hear it too
        case TFBUS_T_DISCOVER:
            for (int i = 0; i < s_nmod; i++) {
                if (!s_mod[i].addr) {
                    uint32_t sn = s_mod[i].sn;
                    const uint8_t p[7] = {(uint8_t)sn, (uint8_t)(sn >> 8), (uint8_t)(sn >> 16),
                                          (uint8_t)(sn >> 24), LOCALIO_HW_TYPE, LOCALIO_FW_TYPE,
                                          LOCALIO_FW_API_VER};
                    return reply(rx, rxcap, p, sizeof p);
                }
            }
            return 0; // all addressed: the next DISCOVER is for the wire
        case TFBUS_T_ADDR_ASSIGN:
            if (plen >= 5) {
                uint32_t sn = pl[0] | pl[1] << 8 | pl[2] << 16 | (uint32_t)pl[3] << 24;
                for (int i = 0; i < s_nmod; i++) {
                    if (s_mod[i].sn == sn) {
                        s_mod[i].addr = pl[4];
                        return reply(rx, rxcap, NULL, 0);
                    }
                }
            }
            return 0;
        default:
            return 0; // SEEK etc.: for the wire
        }
    }

    int i = 0;
    while (i < s_nmod && !(s_mod[i].addr && s_mod[i].addr == dest)) {
        i++;
    }
    if (i == s_nmod) {
        return 0;
    }
    switch (type) {
    case TFBUS_T_RESET:
        s_mod[i].addr = 0;
        return -1;
    case TFBUS_T_STATUS_POLL: {
        if (!sample()) {
            return -1; // no reply: the master takes the module offline
        }
        const uint8_t p[9] = {2, 0x00, 1, 1, (uint8_t)(s_in >> (8 * i)),
                              0x01, 1, 1, (uint8_t)(s_out >> (8 * i))};
        return reply(rx, rxcap, p, sizeof p);
    }
    case TFBUS_T_VERSION_QUERY:
        return reply(rx, rxcap, VERSION, sizeof VERSION);
    case TFBUS_T_MODULE_CMD:
        return module_cmd(i, pl, plen) ? reply(rx, rxcap, NULL, 0) : -1;
    default:
        return reply(rx, rxcap, NULL, 0);
    }
}
