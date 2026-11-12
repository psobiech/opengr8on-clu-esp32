// Host tests for the on-board IO emulation (localio.c): IO_MODULE_DIN_8 modules answering TF-Bus
// frames, against fake expanders and a simulated clock (esp_timer_get_time).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clu_localio.h"
#include "esp_timer.h"
#include "tfbus_frame.h"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- fake clock + expanders
static int64_t s_now = 1000000;
int64_t esp_timer_get_time(void) { return s_now; }

static uint16_t s_written, s_inputs;
static int s_writes, s_reads;
static bool s_write_fail, s_read_fail;

static bool hw_write(uint16_t on, void *ctx)
{
    s_writes++;
    if (s_write_fail) {
        return false;
    }
    s_written = on;
    return true;
}

static bool hw_read(uint16_t *active, void *ctx)
{
    s_reads++;
    if (s_read_fail) {
        return false;
    }
    *active = s_inputs;
    return true;
}

static const localio_hw_t s_hw = {.write = hw_write, .read = hw_read};

// ---------------------------------------------------------------- helpers
static uint8_t f[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];

/** @brief Send a frame; returns the reply length, payload in *pl / *plen when it parses. */
static int send(size_t fl, const uint8_t **pl, size_t *plen)
{
    memset(rx, 0, sizeof rx);
    int n = localio_frame(f, fl, rx, sizeof rx);
    if (n > 0 && pl && !tfbus_parse_response(rx, (size_t)n, pl, plen)) {
        *plen = (size_t)-1;
    }
    return n;
}

static bool is_ack(int n)
{
    return n == 4 && rx[0] == 0xaa && rx[1] == 0x00 && rx[2] == 0x01 && rx[3] == 0xab;
}

/** @brief STATUS_POLL @p addr; returns true and the input / output bytes on a valid reply. */
static bool poll(uint8_t addr, uint8_t *in, uint8_t *out)
{
    const uint8_t *pl;
    size_t plen = 0;
    int n = send(tfbus_status_poll(f, sizeof f, addr), &pl, &plen);
    tfbus_tlv_t r[4];
    if (n <= 0 || tfbus_parse_tlv(pl, plen, r, 4) != 2 || r[0].type != 0 || r[1].type != 1) {
        return false;
    }
    *in = r[0].data[0];
    *out = r[1].data[0];
    return r[0].len == 1 && r[0].meta == 1 && r[1].len == 1 && r[1].meta == 1;
}

/** @brief MODULE_CMD with one ACTIVATE (0x08) / DEACTIVATE (0x09) record. */
static int cmd(uint8_t addr, uint8_t type, uint8_t mask)
{
    const tfbus_tlv_t rec = {.type = type, .meta = 0x01, .len = 1, .data = &mask};
    return send(tfbus_module_cmd(f, sizeof f, addr, &rec, 1), NULL, NULL);
}

int main(void)
{
    const uint32_t sn[LOCALIO_MAX_MODULES] = {0x13ab6c81, 0x22222222};
    const uint8_t *pl;
    size_t plen;
    tfbus_ident_t id;
    uint8_t in, out;
    int n;

    s_written = 0xffff;
    localio_setup(sn, 2, &s_hw);
    check("setup: all outputs off", s_writes == 1 && s_written == 0);

    // ---------------- discovery / addressing
    n = send(tfbus_discover(f, sizeof f), &pl, &plen);
    check("DISCOVER: module 1 answers SN hwType 0x1e fwType 2 fwApiVer 2",
          n > 0 && tfbus_parse_discover(pl, plen, &id) && id.sn == sn[0] && id.hw_type == 0x1e &&
              id.fw_type == 2 && id.fw_api_ver == 2);
    check("ADDR_ASSIGN of another SN: not local -> wire", send(tfbus_addr_assign(f, sizeof f, 0x33333333, 9), NULL, NULL) == 0);
    check("ADDR_ASSIGN module 1 -> ACK", is_ack(send(tfbus_addr_assign(f, sizeof f, sn[0], 5), NULL, NULL)));
    n = send(tfbus_discover(f, sizeof f), &pl, &plen);
    check("DISCOVER: then module 2", n > 0 && tfbus_parse_discover(pl, plen, &id) && id.sn == sn[1]);
    check("ADDR_ASSIGN module 2 -> ACK", is_ack(send(tfbus_addr_assign(f, sizeof f, sn[1], 6), NULL, NULL)));
    check("DISCOVER: all addressed -> wire", send(tfbus_discover(f, sizeof f), NULL, NULL) == 0);
    check("SEEK -> wire", send(tfbus_seek(f, sizeof f), NULL, NULL) == 0);

    // ---------------- polling
    s_inputs = 0x8001;
    check("STATUS_POLL module 1: inputs 1-8, outputs", poll(5, &in, &out) && in == 0x01 && out == 0x00);
    check("STATUS_POLL module 2: inputs 9-16", poll(6, &in, &out) && in == 0x80 && out == 0x00);
    check("STATUS_POLL other address -> wire", send(tfbus_status_poll(f, sizeof f, 7), NULL, NULL) == 0);
    n = send(tfbus_version_query(f, sizeof f, 6), &pl, &plen);
    check("VERSION_QUERY: fwApiVer 2, 2.4, hwVer 3",
          n > 0 && plen == 4 && pl[0] == 2 && pl[1] == 2 && pl[2] == 4 && pl[3] == 3);

    // ---------------- outputs
    int w = s_writes;
    check("ACTIVATE ch 0 on module 2 -> ACK", is_ack(cmd(6, 0x08, 0x01)));
    check("  output 9 driven", s_written == 0x0100 && s_writes == w + 1);
    check("ACTIVATE ch 7 on module 1 -> output 8", is_ack(cmd(5, 0x08, 0x80)) && s_written == 0x0180);
    check("  reported in the module status", poll(5, &in, &out) && out == 0x80 && poll(6, &in, &out) && out == 0x01);
    w = s_writes;
    check("ACTIVATE of an output already on: ACK, no I2C write", is_ack(cmd(5, 0x08, 0x80)) && s_writes == w);
    check("DEACTIVATE ch 0 on module 2", is_ack(cmd(6, 0x09, 0x01)) && s_written == 0x0080);
    {
        const uint8_t on = 0x02, off = 0x80;
        const tfbus_tlv_t recs[2] = {{.type = 0x08, .meta = 1, .len = 1, .data = &on},
                                     {.type = 0x09, .meta = 1, .len = 1, .data = &off}};
        n = send(tfbus_module_cmd(f, sizeof f, 5, recs, 2), NULL, NULL);
        check("MODULE_CMD with two records applied in one write", is_ack(n) && s_written == 0x0002);
        const uint8_t param[4] = {0, 1, 0, 0};
        const tfbus_tlv_t p = {.type = 0x0f, .meta = 0, .len = 4, .data = param};
        w = s_writes;
        n = send(tfbus_module_cmd(f, sizeof f, 5, &p, 1), NULL, NULL);
        check("MODULE_PARAM: ACK, outputs untouched", is_ack(n) && s_writes == w && s_written == 0x0002);
    }
    s_write_fail = true;
    check("output write failure -> no reply (master retries)", cmd(5, 0x08, 0x04) < 0);
    s_write_fail = false;
    check("  failed write not reported as switched", poll(5, &in, &out) && out == 0x02);
    check("other frame types -> ACK", is_ack(send(tfbus_addr_confirm(f, sizeof f, 5), NULL, NULL)));
    {
        size_t fl = tfbus_status_poll(f, sizeof f, 5);
        f[fl - 1] ^= 0xff;
        check("bad checksum -> ignored", localio_frame(f, fl, rx, sizeof rx) == 0);
    }
    check("reply that does not fit rxcap -> not answered",
          localio_frame(f, tfbus_status_poll(f, sizeof f, 5), rx, 6) == 0);

    // ---------------- input sampling / debounce
    s_now += LOCALIO_SAMPLE_US;
    poll(5, &in, &out);
    int r = s_reads;
    s_inputs = 0x0003;
    poll(5, &in, &out);
    check("inputs not re-read within the sample period", s_reads == r && in == 0x01);
    s_now += LOCALIO_SAMPLE_US;
    poll(5, &in, &out);
    check("first sample of a change not taken (debounce)", s_reads == r + 1 && in == 0x01);
    s_now += LOCALIO_SAMPLE_US;
    poll(5, &in, &out);
    check("second equal sample taken", in == 0x03 && poll(6, &in, &out) && in == 0x00);
    s_inputs = 0x0001;
    s_now += LOCALIO_SAMPLE_US;
    poll(5, &in, &out);
    s_inputs = 0x0003;
    s_now += LOCALIO_SAMPLE_US;
    poll(5, &in, &out);
    check("one-sample glitch ignored", in == 0x03);

    s_read_fail = true;
    s_now += LOCALIO_SAMPLE_US;
    check("input read failure -> no reply (module goes offline)",
          send(tfbus_status_poll(f, sizeof f, 5), NULL, NULL) < 0);
    r = s_reads;
    s_now += 500000;
    check("no retry within 1 s", send(tfbus_status_poll(f, sizeof f, 5), NULL, NULL) < 0 && s_reads == r);
    s_read_fail = false;
    s_inputs = 0x0100;
    s_now += 600000;
    check("retried after 1 s; first sample after a failure taken at once",
          poll(6, &in, &out) && in == 0x01 && s_reads == r + 1);

    // ---------------- reset
    check("unicast RESET -> owned, no reply", send(tfbus_reset(f, sizeof f, 6), NULL, NULL) < 0);
    check("  module 2 unaddressed", send(tfbus_status_poll(f, sizeof f, 6), NULL, NULL) == 0 && poll(5, &in, &out));
    check("RESET broadcast -> also for the wire", send(tfbus_reset(f, sizeof f, TFBUS_BROADCAST), NULL, NULL) == 0);
    check("  both modules unaddressed, outputs kept",
          send(tfbus_status_poll(f, sizeof f, 5), NULL, NULL) == 0 && s_written == 0x0002);
    n = send(tfbus_discover(f, sizeof f), &pl, &plen);
    check("  DISCOVER answered again", n > 0 && tfbus_parse_discover(pl, plen, &id) && id.sn == sn[0]);

    // ---------------- one module (KinCony F8 / KC868-A8v3: 8 relays + 8 inputs)
    s_written = 0xffff;
    localio_setup(&sn[1], 1, &s_hw);
    check("one module: outputs off at setup", s_written == 0);
    n = send(tfbus_discover(f, sizeof f), &pl, &plen);
    check("one module: DISCOVER answered by it", n > 0 && tfbus_parse_discover(pl, plen, &id) && id.sn == sn[1]);
    check("one module: ADDR_ASSIGN", is_ack(send(tfbus_addr_assign(f, sizeof f, sn[1], 3), NULL, NULL)));
    check("one module: no second module to discover", send(tfbus_discover(f, sizeof f), NULL, NULL) == 0);
    check("one module: other SN not local", send(tfbus_addr_assign(f, sizeof f, sn[0], 4), NULL, NULL) == 0);
    s_inputs = 0x00a5;
    s_now += 2 * LOCALIO_SAMPLE_US;
    check("one module: inputs from bits 0-7", poll(3, &in, &out) && in == 0xa5);
    check("one module: ACTIVATE ch 7 -> bit 7", is_ack(cmd(3, 0x08, 0x80)) && s_written == 0x0080);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
