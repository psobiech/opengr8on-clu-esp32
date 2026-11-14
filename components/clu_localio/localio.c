/**
 * @file localio.c
 * @brief TF-Bus module side of the on-board IO: emulated IO_MODULE_DIN_8 modules.
 *
 * Answers what a real IO_MODULE_DIN_8 answers (bus part in tfbus_vmod.c, wire format in
 * drv_digital_io.c):
 * @verbatim
 *   STATUS_POLL    : 02 | 00 01 01 [inputs] | 01 01 01 [outputs]  (no reply: expander unreadable)
 *   VERSION_QUERY  : fwApiVer verA verB hwVer
 *   MODULE_CMD     : ACTIVATE (08) / DEACTIVATE (09) [bitmask] drive outputs, rest ACKed
 * @endverbatim
 * Frames for other addresses / serials return 0 and go on the RS485 wire. Pure C (tfbus_frame,
 * tfbus_vmod, esp_timer only): host-tested.
 */
#include "clu_localio.h"

#include <stdint.h>
#include <string.h>

#include "esp_timer.h"
#include "tfbus_frame.h"
#include "tfbus_vmod.h"

#define FAIL_RETRY_US 1000000 // after a failed input read, try the hardware again after 1 s

static tfbus_vmod_t s_mod[LOCALIO_MAX_MODULES];
static int s_nmod;
static const localio_hw_t *s_hw;
static uint16_t s_out;      // outputs as last driven
static uint16_t s_in;       // debounced inputs
static uint16_t s_raw;      // last raw sample
static bool s_have_in;      // s_in / s_raw valid
static int64_t s_sampled;   // time of the last good sample
static int64_t s_failed;    // time of the last failed read (0 = none since the last good one)

static int din8_status(tfbus_vmod_t *module, uint8_t *status_buffer, size_t cap);
static bool din8_command(tfbus_vmod_t *module, uint8_t type, const uint8_t *payload, size_t plen);

static const tfbus_vmod_ops_t s_ops = {
    .hw_type = LOCALIO_HW_TYPE,
    .fw_type = LOCALIO_FW_TYPE,
    .fw_api_ver = LOCALIO_FW_API_VER,
    .version = {LOCALIO_FW_API_VER, 2, 4, 3}, // fwVer "2.2.4", like a real module
    .status = din8_status,
    .command = din8_command,
};

void localio_setup(const uint32_t *serial_numbers, int module_count, const localio_hw_t *hardware)
{
    s_nmod = module_count < 1 ? 1 : module_count > LOCALIO_MAX_MODULES ? LOCALIO_MAX_MODULES : module_count;
    for (int i = 0; i < s_nmod; i++) {
        s_mod[i] = (tfbus_vmod_t){.sn = serial_numbers[i], .ops = &s_ops, .ctx = (void *)(intptr_t)i};
    }
    s_hw = hardware;
    s_out = 0;
    s_have_in = false;
    s_failed = 0;
    s_hw->write(0, s_hw->ctx);
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

/** @brief Module @p m: byte i of the 16-bit IO words. */
static int index_of(const tfbus_vmod_t *module)
{
    return (int)(intptr_t)module->ctx;
}

static int din8_status(tfbus_vmod_t *module, uint8_t *status_buffer, size_t cap)
{
    if (!sample()) {
        return -1; // no reply: the master takes the module offline
    }
    int module_index = index_of(module);
    tfbus_din8_status((uint8_t)(s_in >> (8 * module_index)), (uint8_t)(s_out >> (8 * module_index)), status_buffer);
    return TFBUS_DIN8_STATUS_LEN;
}

/** @brief MODULE_CMD: ACTIVATE / DEACTIVATE this module's outputs; false (no reply) on a write error. */
static bool din8_command(tfbus_vmod_t *module, uint8_t type, const uint8_t *payload, size_t plen)
{
    if (type != TFBUS_T_MODULE_CMD) {
        return true;
    }
    int module_index = index_of(module);
    uint8_t mine = (uint8_t)(s_out >> (8 * module_index));
    if (!tfbus_din8_cmd(payload, plen, &mine)) {
        return false;
    }
    uint16_t out = (uint16_t)((s_out & ~(0xffu << (8 * module_index))) | (uint16_t)(mine << (8 * module_index)));
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
    return s_hw ? tfbus_vmod_frame(s_mod, s_nmod, tx, txlen, rx, rxcap) : 0;
}
