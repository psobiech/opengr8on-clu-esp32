// Host tests for the RTC: time register encoding (pcf85063.c, ds3231.c) and the I2C driver
// (clu_rtc.c and the shared bus clu_i2c.c, included: state is static) against fake chips with a
// register file each.
#include <stdio.h>
#include <string.h>

#define TAG I2C_TAG // both sources define TAG
#include "../../components/clu_i2c/clu_i2c.c"
#undef TAG
#include "../../components/clu_rtc/clu_rtc.c"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- fake chip
static struct host_i2c_bus { int sda, scl, pullup; } s_bus_obj;
static struct host_i2c_dev { int addr, hz; } s_dev_obj;
static uint8_t s_reg[0x12]; // PCF85063 (0x51)
static uint8_t s_ds[0x13];  // DS3231 (0x68)
static int s_chip_addr = PCF85063_I2C_ADDR; // the chip on the bus
static bool s_chip = true, s_bus_fail, s_xfer_fail;

/** @brief Register file of the addressed device, NULL if nothing at that address. */
static uint8_t *regs(i2c_master_dev_handle_t dev, size_t *n)
{
    if (dev->addr == DS3231_I2C_ADDR) {
        *n = sizeof s_ds;
        return s_ds;
    }
    *n = sizeof s_reg;
    return s_reg;
}
static int s_buses, s_rms, s_writes;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *c, i2c_master_bus_handle_t *bus)
{
    if (s_bus_fail) {
        return ESP_FAIL;
    }
    s_buses++;
    s_bus_obj = (struct host_i2c_bus){c->sda_io_num, c->scl_io_num, c->flags.enable_internal_pullup};
    *bus = &s_bus_obj;
    return ESP_OK;
}
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int t)
{
    return s_chip && addr == s_chip_addr ? ESP_OK : ESP_ERR_TIMEOUT;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *c,
                                    i2c_master_dev_handle_t *dev)
{
    s_dev_obj = (struct host_i2c_dev){c->device_address, (int)c->scl_speed_hz};
    *dev = &s_dev_obj;
    return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) { s_rms++; return ESP_OK; }
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wn, int t)
{
    size_t n;
    uint8_t *rf = regs(dev, &n);
    if (s_xfer_fail || wn < 1 || w[0] + wn - 1 > n) {
        return ESP_FAIL;
    }
    memcpy(rf + w[0], w + 1, wn - 1);
    s_writes++;
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wn, uint8_t *r,
                                      size_t rn, int t)
{
    size_t n;
    uint8_t *rf = regs(dev, &n);
    if (s_xfer_fail || wn != 1 || w[0] + rn > n) {
        return ESP_FAIL;
    }
    memcpy(r, rf + w[0], rn);
    return ESP_OK;
}

static void reset_driver(void)
{
    s_nbus = 0; // clu_i2c: forget the shared buses
    s_dev = NULL;
    s_buses = s_rms = s_writes = 0;
}

// ---------------------------------------------------------------- tests
static void test_codec(void)
{
    // 2025-06-15 13:45:30 UTC (Sunday) = 1749995130
    uint8_t r[7];
    check("encode: in range", pcf85063_encode(1749995130, r));
    const uint8_t want[7] = {0x30, 0x45, 0x13, 0x15, 0x00, 0x06, 0x25};
    check("encode: BCD fields, weekday 0 = Sunday", !memcmp(r, want, 7));
    time_t t = 0;
    check("decode: round trip", pcf85063_decode(r, &t) && t == 1749995130);

    // leap day, end of century of the chip's range, first second of its range
    const time_t samples[] = {951782400 /* 2000-02-29 */, 946684800 /* 2000-01-01 */,
                              4102444799 /* 2099-12-31 23:59:59 */, 1709251199 /* 2024-02-29 23:59:59 */};
    bool ok = true;
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        ok &= pcf85063_encode(samples[i], r) && pcf85063_decode(r, &t) && t == samples[i];
    }
    check("round trip: leap day, 2000-01-01, 2099-12-31, 2024-02-29", ok);
    // every day of 2024..2027 at 23:59:59
    ok = true;
    for (time_t d = 1704067200; d < 1830297600; d += 86400) {
        ok &= pcf85063_encode(d + 86399, r) && pcf85063_decode(r, &t) && t == d + 86399;
    }
    check("round trip: every day 2024..2027", ok);
    pcf85063_encode(1704067200, r); // 2024-01-01 was a Monday
    check("weekday: 2024-01-01 Monday", r[4] == 1);

    check("encode: before 2000 refused", !pcf85063_encode(946684799, r));
    check("encode: after 2099 refused", !pcf85063_encode(4102444800, r));

    uint8_t bad[7];
    memcpy(bad, want, 7);
    bad[0] |= PCF85063_SECONDS_OS;
    check("decode: OS flag (power lost) -> invalid", !pcf85063_decode(bad, &t));
    memcpy(bad, want, 7);
    bad[0] = 0x5a;
    check("decode: non-BCD seconds -> invalid", !pcf85063_decode(bad, &t));
    memcpy(bad, want, 7);
    bad[3] = 0x31;
    check("decode: 31 June -> invalid", !pcf85063_decode(bad, &t));
    memcpy(bad, want, 7);
    bad[5] = 0x13;
    check("decode: month 13 -> invalid", !pcf85063_decode(bad, &t));
    memcpy(bad, want, 7);
    bad[2] = 0x24;
    check("decode: hour 24 -> invalid", !pcf85063_decode(bad, &t));
    const uint8_t feb29_2025[7] = {0, 0, 0, 0x29, 0, 0x02, 0x25};
    check("decode: 29 Feb of a non-leap year -> invalid", !pcf85063_decode(feb29_2025, &t));
    memcpy(bad, want, 7);
    bad[4] = 0xff; // weekday not used for the epoch, unused bits ignored
    bad[1] |= 0x80;
    check("decode: unused bits and weekday ignored", pcf85063_decode(bad, &t) && t == 1749995130);
}

static void test_driver(void)
{
    time_t t;
    reset_driver();
    check("no RTC configured (pin -1): no bus", !clu_rtc_init(CLU_RTC_PCF85063, -1, 38) && !clu_rtc_init(CLU_RTC_PCF85063, 39, -1) && s_buses == 0);
    check("not initialised: get / set fail", !clu_rtc_get(&t) && !clu_rtc_set(1750000000));

    s_bus_fail = true;
    check("bus creation failure -> false", !clu_rtc_init(CLU_RTC_PCF85063, 39, 38));
    s_bus_fail = false;

    s_chip = false;
    check("no chip answers -> false", !clu_rtc_init(CLU_RTC_PCF85063, 39, 38) && s_buses == 1);
    s_chip = true;
    check("chip found on the retry, bus reused", clu_rtc_init(CLU_RTC_PCF85063, 39, 38) && s_buses == 1);
    check("bus: Kconfig pins, internal pull-ups; device 0x51 at 100 kHz",
          s_bus_obj.sda == 39 && s_bus_obj.scl == 38 && s_bus_obj.pullup && s_dev_obj.addr == 0x51 &&
              s_dev_obj.hz == 100000);
    check("init again: no-op", clu_rtc_init(CLU_RTC_PCF85063, 39, 38) && s_buses == 1);

    reset_driver();
    memset(s_reg, 0, sizeof s_reg);
    s_reg[0] = 0x23; // STOP | 12-hour | CAP_SEL
    clu_rtc_init(CLU_RTC_PCF85063, 39, 38);
    check("init: STOP and 12-hour mode cleared, CAP_SEL kept", s_reg[0] == 0x01);
    reset_driver();
    clu_rtc_init(CLU_RTC_PCF85063, 39, 38);
    check("init: Control_1 already fine -> not written", s_writes == 0);

    reset_driver();
    s_xfer_fail = true;
    check("init: Control_1 read failure -> false, device removed", !clu_rtc_init(CLU_RTC_PCF85063, 39, 38) && s_rms == 1);
    s_xfer_fail = false;
    clu_rtc_init(CLU_RTC_PCF85063, 39, 38);

    s_reg[PCF85063_REG_SECONDS] = 0x80; // power-up state: OS set
    check("get: OS flag set -> invalid", !clu_rtc_get(&t));
    check("set: before 2024 refused", !clu_rtc_set(1700000000) && s_writes == 0);
    check("set: written at Seconds..Years, OS cleared", clu_rtc_set(1749995130) && s_reg[4] == 0x30 &&
                                                            s_reg[10] == 0x25 && !(s_reg[4] & 0x80));
    check("get: time back", clu_rtc_get(&t) && t == 1749995130);
    s_reg[10] = 0x23; // 2023
    check("get: before 2024 -> not set", !clu_rtc_get(&t));
    s_xfer_fail = true;
    check("get / set: I2C failure -> false", !clu_rtc_get(&t) && !clu_rtc_set(1749995130));
    s_xfer_fail = false;
}

static void test_ds3231_codec(void)
{
    // 2025-06-15 13:45:30 UTC (Sunday) = 1749995130
    uint8_t r[7];
    time_t t = 0;
    check("DS3231 encode: in range", ds3231_encode(1749995130, r));
    const uint8_t want[7] = {0x30, 0x45, 0x13, 0x01, 0x15, 0x06, 0x25};
    check("DS3231 encode: BCD fields, day 1 = Sunday, date, 24-hour, century clear", !memcmp(r, want, 7));
    check("DS3231 decode: round trip", ds3231_decode(r, &t) && t == 1749995130);
    bool ok = true;
    for (time_t d = 1704067200; d < 1830297600; d += 86400) {
        ok &= ds3231_encode(d + 86399, r) && ds3231_decode(r, &t) && t == d + 86399 && r[3] >= 1 && r[3] <= 7;
    }
    check("DS3231 round trip: every day 2024..2027, day 1..7", ok);
    ds3231_encode(1704067200, r); // 2024-01-01 was a Monday
    check("DS3231 day: 2024-01-01 Monday = 2", r[3] == 2);
    check("DS3231 encode: outside 2000..2099 refused", !ds3231_encode(946684799, r) && !ds3231_encode(4102444800, r));
    uint8_t bad[7];
    memcpy(bad, want, 7);
    bad[2] = 0x40 | 0x01; // 12-hour mode, 1 AM
    check("DS3231 decode: 12-hour mode -> invalid", !ds3231_decode(bad, &t));
    memcpy(bad, want, 7);
    bad[5] |= DS3231_MONTH_CENTURY;
    check("DS3231 decode: century bit (past 2099) -> invalid", !ds3231_decode(bad, &t));
    memcpy(bad, want, 7);
    bad[4] = 0x31; // 31 June
    check("DS3231 decode: 31 June -> invalid", !ds3231_decode(bad, &t));
    memcpy(bad, want, 7);
    bad[3] = 0x07; // day of week not used for the epoch
    check("DS3231 decode: day of week ignored", ds3231_decode(bad, &t) && t == 1749995130);
}

static void test_ds3231_driver(void)
{
    time_t t;
    reset_driver();
    s_chip_addr = DS3231_I2C_ADDR;
    check("DS3231: PCF85063 configured, DS3231 on the bus -> not found", !clu_rtc_init(CLU_RTC_PCF85063, 8, 18));
    memset(s_ds, 0, sizeof s_ds);
    s_ds[DS3231_REG_CONTROL] = 0x9c; // EOSC | power-up defaults
    s_ds[DS3231_REG_STATUS] = 0x88;  // OSF | EN32kHz
    check("DS3231: found at 0x68, same bus reused", clu_rtc_init(CLU_RTC_DS3231, 8, 18) && s_buses == 1 &&
                                                          s_dev_obj.addr == 0x68 && s_bus_obj.sda == 8);
    check("DS3231 init: EOSC cleared (runs on battery), other Control bits kept", s_ds[DS3231_REG_CONTROL] == 0x1c);
    check("DS3231 get: OSF set (power lost) -> invalid", !clu_rtc_get(&t));
    check("DS3231 set: written at Seconds..Year", clu_rtc_set(1749995130) && s_ds[0] == 0x30 && s_ds[3] == 0x01 &&
                                                      s_ds[4] == 0x15 && s_ds[6] == 0x25);
    check("DS3231 set: OSF cleared, EN32kHz kept", s_ds[DS3231_REG_STATUS] == 0x08);
    check("DS3231 get: time back", clu_rtc_get(&t) && t == 1749995130);
    s_ds[2] = 0x41; // 12-hour mode
    check("DS3231 get: 12-hour mode -> invalid (rewritten by the next set)", !clu_rtc_get(&t));
    check("DS3231 set: back to 24-hour", clu_rtc_set(1749995130) && s_ds[2] == 0x13 && clu_rtc_get(&t));
    s_xfer_fail = true;
    check("DS3231 get / set: I2C failure -> false", !clu_rtc_get(&t) && !clu_rtc_set(1749995130));
    s_xfer_fail = false;

    reset_driver();
    s_ds[DS3231_REG_CONTROL] = 0x1c;
    int w = s_writes;
    clu_rtc_init(CLU_RTC_DS3231, 8, 18);
    check("DS3231 init: Control already fine -> not written", s_writes == w);
    s_chip_addr = PCF85063_I2C_ADDR;
}

static void test_shared_bus(void)
{
    reset_driver();
    i2c_master_bus_handle_t a = clu_i2c_bus(8, 18);
    check("clu_i2c: same pins -> same bus, created once", a && clu_i2c_bus(8, 18) == a && s_buses == 1);
    check("clu_i2c: pin of another pair -> refused", !clu_i2c_bus(8, 19) && !clu_i2c_bus(18, 5) && s_buses == 1);
    check("clu_i2c: second pair -> second bus", clu_i2c_bus(39, 38) && s_buses == 2);
    check("clu_i2c: no third controller", !clu_i2c_bus(1, 2) && s_buses == 2);
    check("clu_i2c: pin < 0 -> none", !clu_i2c_bus(-1, 2));
}

int main(void)
{
    test_codec();
    test_driver();
    test_ds3231_codec();
    test_ds3231_driver();
    test_shared_bus();
    printf(failures ? "FAILED (%d failures)\n" : "PASSED (%d failures)\n", failures);
    return failures != 0;
}
