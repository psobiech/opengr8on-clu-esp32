// Host tests for the on-board IO board glue (clu_localio.c, included: state is static): module
// serial in NVS / menuconfig, and the PCF8575 pin mapping of one board, against a fake expander.
// Built twice: -DBOARD_F8 (relays P10-P17, inputs P00-P07, 0x24) and -DBOARD_A8V3 (relays
// P00-P07, inputs P10-P17, 0x22).
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_CLU_LOCAL_IO 1
#define CONFIG_CLU_LOCAL_IO_SDA_GPIO 8
#define CONFIG_CLU_LOCAL_IO_SCL_GPIO 18
#define CONFIG_CLU_LOCAL_IO_OUT_ACTIVE_LOW 1
#define CONFIG_CLU_LOCAL_IO_IN_ACTIVE_LOW 1
#define CONFIG_CLU_LOCAL_IO_SN cfg_sn
static const char *cfg_sn = "";
#if BOARD_F8
#define CONFIG_CLU_LOCAL_IO_ADDR 0x24
#define CONFIG_CLU_LOCAL_IO_OUT_P1 1
#define BOARD "F8"
#else
#define CONFIG_CLU_LOCAL_IO_ADDR 0x22
#define CONFIG_CLU_LOCAL_IO_OUT_P0 1
#define BOARD "A8v3"
#endif

#include "tfbus_frame.h"
#include "fake_nvs.h"
#include "../../components/clu_localio/clu_localio.c"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s: %s\n", cond ? "ok  " : "FAIL", BOARD, name);
    failures += !cond;
}

// ---------------------------------------------------------------- fakes
static int64_t s_now = 1000000;
int64_t esp_timer_get_time(void) { return s_now; }

static uint32_t s_rand = 0x8badf00d;
void clu_random(uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(s_rand >> (8 * (i % 4)));
    }
}

static tfbus_uart_local_fn_t s_local;
void tfbus_uart_set_local(tfbus_uart_local_fn_t fn) { s_local = fn; }

// PCF8575: latch written by the CLU (P00-P07 = byte 0, P10-P17 = byte 1); a pin reads low when
// the latch drives it low or the outside world pulls it low.
static struct host_i2c_bus { int sda, scl; } s_bus_obj;
static struct host_i2c_dev { int addr; } s_dev_obj;
static uint16_t s_latch = 0xffff, s_pulled_low;
static bool s_chip = true;
static int s_bus_addr, s_buses;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *c, i2c_master_bus_handle_t *bus)
{
    s_buses++;
    s_bus_obj = (struct host_i2c_bus){c->sda_io_num, c->scl_io_num};
    *bus = &s_bus_obj;
    return ESP_OK;
}
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int t)
{
    return s_chip && addr == s_dev_obj.addr ? ESP_OK : ESP_ERR_TIMEOUT;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *c,
                                    i2c_master_dev_handle_t *dev)
{
    s_dev_obj.addr = c->device_address;
    s_bus_addr = c->device_address;
    *dev = &s_dev_obj;
    return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) { return ESP_OK; }
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wn, int t)
{
    if (!s_chip || wn != 2) {
        return ESP_FAIL;
    }
    s_latch = (uint16_t)(w[0] | w[1] << 8);
    return ESP_OK;
}
esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *r, size_t rn, int t)
{
    if (!s_chip || rn != 2) {
        return ESP_FAIL;
    }
    uint16_t v = s_latch & ~s_pulled_low;
    r[0] = (uint8_t)v;
    r[1] = (uint8_t)(v >> 8);
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wn, uint8_t *r,
                                      size_t rn, int t)
{
    return ESP_FAIL;
}

// ---------------------------------------------------------------- helpers
static uint8_t f[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];

static int bus(size_t fl)
{
    return s_local(f, fl, rx, sizeof rx);
}

static bool poll(uint8_t addr, uint8_t *in, uint8_t *out)
{
    const uint8_t *pl;
    size_t plen;
    tfbus_tlv_t r[4];
    int n = bus(tfbus_status_poll(f, sizeof f, addr));
    if (n <= 0 || !tfbus_parse_response(rx, (size_t)n, &pl, &plen) || tfbus_parse_tlv(pl, plen, r, 4) != 2) {
        return false;
    }
    *in = r[0].data[0];
    *out = r[1].data[0];
    return true;
}

static int cmd(uint8_t addr, uint8_t type, uint8_t mask)
{
    const tfbus_tlv_t rec = {.type = type, .meta = 0x01, .len = 1, .data = &mask};
    return bus(tfbus_module_cmd(f, sizeof f, addr, &rec, 1));
}

/** @brief Relay half / input half of a 16-bit pin word for this board. */
#if CONFIG_CLU_LOCAL_IO_OUT_P0
static uint8_t relay_pins(uint16_t v) { return (uint8_t)v; }
static uint16_t input_pins(uint8_t v) { return (uint16_t)(v << 8); }
#else
static uint8_t relay_pins(uint16_t v) { return (uint8_t)(v >> 8); }
static uint16_t input_pins(uint8_t v) { return v; }
#endif

int main(void)
{
    uint32_t sn;
    uint8_t in, out;
    const uint8_t *pl;
    size_t plen;
    tfbus_ident_t id;

    // ---------------- start: serial generated + stored, chip set up, hook installed
    nvs_reset();
    i2c_master_bus_handle_t rtc_bus = clu_i2c_bus(8, 18); // the RTC got the bus first (boot order)
    clu_localio_start();
    check("I2C bus shared with the RTC (created once)", rtc_bus && s_buses == 1);
    check("hook installed, PCF8575 at the board address, I2C 8/18",
          s_local != NULL && s_bus_addr == CONFIG_CLU_LOCAL_IO_ADDR && s_bus_obj.sda == 8 && s_bus_obj.scl == 18);
    check("relays off, input pins high", s_latch == 0xffff);
    check("random serial stored in NVS", nvs_get_u32(1, "sn1", &sn) == ESP_OK && sn == (0x8badf00d & 0x7fffffff));
    int n = bus(tfbus_discover(f, sizeof f));
    check("one module: DISCOVER reports the stored serial",
          n > 0 && tfbus_parse_response(rx, (size_t)n, &pl, &plen) && tfbus_parse_discover(pl, plen, &id) &&
              id.sn == sn && id.hw_type == 0x1e);
    bus(tfbus_addr_assign(f, sizeof f, sn, 1));
    check("only one module", bus(tfbus_discover(f, sizeof f)) == 0);

    // ---------------- relays: DOUT channel k -> relay k+1, active low; input half stays high
    check("ACTIVATE ch 0 -> relay 1 pin low", cmd(1, 0x08, 0x01) > 0 && relay_pins(s_latch) == 0xfe);
    check("  input pins still high", (s_latch & input_pins(0xff)) == input_pins(0xff));
    check("ACTIVATE ch 7 -> relay 8 pin low", cmd(1, 0x08, 0x80) > 0 && relay_pins(s_latch) == 0x7e);
    check("DEACTIVATE ch 0", cmd(1, 0x09, 0x01) > 0 && relay_pins(s_latch) == 0x7f);

    // ---------------- inputs: DI k+1 pulled low -> DIN channel k on
    s_pulled_low = input_pins(0x05); // DI1, DI3
    s_now += 2 * LOCALIO_SAMPLE_US;
    poll(1, &in, &out);
    s_now += LOCALIO_SAMPLE_US;
    check("DI1 + DI3 active -> inputs 0x05, relay 8 reported on", poll(1, &in, &out) && in == 0x05 && out == 0x80);
    s_pulled_low = 0;
    s_now += LOCALIO_SAMPLE_US;
    poll(1, &in, &out);
    s_now += LOCALIO_SAMPLE_US;
    check("relays on do not read back as inputs", poll(1, &in, &out) && in == 0x00);

    // ---------------- expander gone
    s_chip = false;
    s_now += LOCALIO_SAMPLE_US;
    check("expander not answering -> no reply", bus(tfbus_status_poll(f, sizeof f, 1)) < 0);
    s_chip = true;

    // ---------------- serial: kept across restarts, menuconfig overrides
    s_rand = 0x12345678;
    clu_localio_start();
    n = bus(tfbus_discover(f, sizeof f));
    check("restart: same serial from NVS",
          n > 0 && tfbus_parse_response(rx, (size_t)n, &pl, &plen) && tfbus_parse_discover(pl, plen, &id) && id.sn == sn);
    cfg_sn = "330000001";
    clu_localio_start();
    n = bus(tfbus_discover(f, sizeof f));
    uint32_t stored;
    check("menuconfig serial overrides and is stored",
          n > 0 && tfbus_parse_response(rx, (size_t)n, &pl, &plen) && tfbus_parse_discover(pl, plen, &id) &&
              id.sn == 330000001 && nvs_get_u32(1, "sn1", &stored) == ESP_OK && stored == 330000001);
    cfg_sn = "x";
    clu_localio_start();
    n = bus(tfbus_discover(f, sizeof f));
    check("invalid menuconfig serial ignored, NVS value kept",
          n > 0 && tfbus_parse_response(rx, (size_t)n, &pl, &plen) && tfbus_parse_discover(pl, plen, &id) &&
              id.sn == 330000001);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
