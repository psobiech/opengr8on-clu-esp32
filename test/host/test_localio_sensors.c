// Host tests for the on-board analog inputs / 1-Wire sensors as TF-Bus modules (localio_sensors.c,
// included): ANALOG_DIN + ONE_WIRE modules answering frames, the sensor list across searches, read failures,
// search requests from a bus RESET / RefreshSensors, CRC8 and the DS18S20 / DS18B20 decoding.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tfbus_frame.h"
#include "tfbus_vmod.h"
// Included (not linked): the test sees the mutex stub's lock counter of the same unit.
#include "../../components/clu_localio/localio_sensors.c"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- search requests
static int s_requests, s_replace_requests;
static void request_search(bool replace_list)
{
    s_requests++;
    s_replace_requests += replace_list;
}

// ---------------------------------------------------------------- helpers
static uint8_t f[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];
static const uint8_t *pl;
static size_t plen;

/** @brief Send a frame; returns the reply length, its payload in pl / plen. */
static int send(size_t frame_length)
{
    memset(rx, 0, sizeof rx);
    plen = 0;
    int n = localio_sensors_frame(f, frame_length, rx, sizeof rx);
    if (n > 0 && !tfbus_parse_response(rx, (size_t)n, &pl, &plen)) {
        plen = (size_t)-1;
    }
    return n;
}

/** @brief DISCOVER; true with the identity of the module that answered. */
static bool discover(tfbus_ident_t *id)
{
    return send(tfbus_discover(f, sizeof f)) > 0 && tfbus_parse_discover(pl, plen, id);
}

/** @brief STATUS_POLL @p addr; the first record of the reply (type 0xff if none). */
static tfbus_tlv_t poll(uint8_t addr)
{
    tfbus_tlv_t r[2] = {{.type = 0xff}};
    if (send(tfbus_status_poll(f, sizeof f, addr)) <= 0 || tfbus_parse_tlv(pl, plen, r, 2) < 1) {
        r[0].type = 0xff;
    }
    return r[0];
}

static int16_t le16(const uint8_t *data) { return (int16_t)(data[0] | data[1] << 8); }

/** @brief Scratchpad with a valid CRC. */
static void scratchpad(uint8_t pad[9], uint8_t b0, uint8_t b1, uint8_t config, uint8_t remain, uint8_t per_degree)
{
    const uint8_t bytes[8] = {b0, b1, 0x4b, 0x46, config, 0xff, remain, per_degree};
    memcpy(pad, bytes, 8);
    pad[8] = localio_onewire_crc8(pad, 8);
}

int main(void)
{
    // ---------------- CRC8, decoding, serial
    const uint8_t maxim_rom[7] = {0x02, 0x1c, 0xb8, 0x01, 0x00, 0x00, 0x00};
    check("CRC8: Maxim AN27 example ROM -> 0xA2", localio_onewire_crc8(maxim_rom, 7) == 0xa2);
    uint8_t pad[9];
    int16_t centi = 0;
    scratchpad(pad, 0x91, 0x01, 0x7f, 0x0c, 0x10);
    check("DS18B20 12 bit: 0x0191 -> 25.06 C", localio_ds18x20_temperature(0x28, pad, &centi) && centi == 2506);
    scratchpad(pad, 0x5e, 0xff, 0x7f, 0x0c, 0x10);
    check("DS18B20 12 bit: 0xFF5E -> -10.13 C", localio_ds18x20_temperature(0x28, pad, &centi) && centi == -1013);
    scratchpad(pad, 0x91, 0x01, 0x1f, 0x0c, 0x10);
    check("DS18B20 9 bit: undefined low bits dropped -> 25.00 C",
          localio_ds18x20_temperature(0x28, pad, &centi) && centi == 2500);
    scratchpad(pad, 0x32, 0x00, 0xff, 0x0c, 0x10);
    check("DS18S20: 0x0032, COUNT_REMAIN 12 / 16 -> 25.00 C",
          localio_ds18x20_temperature(0x10, pad, &centi) && centi == 2500);
    scratchpad(pad, 0xff, 0xff, 0xff, 0x08, 0x10);
    check("DS18S20: 0xFFFF, COUNT_REMAIN 8 / 16 -> -0.75 C",
          localio_ds18x20_temperature(0x10, pad, &centi) && centi == -75);
    pad[8] ^= 1;
    check("bad CRC rejected", !localio_ds18x20_temperature(0x10, pad, &centi));
    memset(pad, 0, sizeof pad);
    check("all-zero read (bus shorted) rejected", !localio_ds18x20_temperature(0x28, pad, &centi));
    memset(pad, 0xff, sizeof pad);
    check("all-ones read (nobody answered) rejected", !localio_ds18x20_temperature(0x28, pad, &centi));
    scratchpad(pad, 0x91, 0x01, 0x7f, 0x0c, 0x10);
    check("unknown family rejected", !localio_ds18x20_temperature(0x22, pad, &centi));
    const uint64_t ds18b20 = 0x5a00123456789028ULL, ds18s20 = 0x7700000000000110ULL, ds2438 = 0x110000000000aa26ULL;
    check("module serial: low 31 bits of the 48-bit serial", localio_sensor_serial(ds18b20) == 0x34567890);
    check("module serial never 0", localio_sensor_serial(0xff00000000000028ULL) == 1);

    // ---------------- ANALOG_DIN alone (no sensors yet)
    localio_sensors_setup(0x19000001, request_search);
    tfbus_ident_t id;
    check("DISCOVER: ANALOG_DIN 0x19 fw 2 api 2",
          discover(&id) && id.sn == 0x19000001 && id.hw_type == 0x19 && id.fw_type == 2 && id.fw_api_ver == 2);
    send(tfbus_addr_assign(f, sizeof f, 0x19000001, 1));
    check("then nothing else to discover", send(tfbus_discover(f, sizeof f)) == 0);
    localio_sensors_set_input(0, 1500);
    localio_sensors_set_input(6, 20000);
    localio_sensors_set_input(7, 1); // past the 7 inputs: ignored
    tfbus_tlv_t r[2];
    check("status: inputs record + hub record",
          send(tfbus_status_poll(f, sizeof f, 1)) > 0 && plen == TFBUS_ANALOG_STATUS_LEN &&
              tfbus_parse_tlv(pl, plen, r, 2) == 2 && r[0].type == 0x20 && r[0].len == 14 &&
              le16(r[0].data) == 1500 && (uint16_t)le16(r[0].data + 12) == 20000 && r[1].type == 0x21 &&
              r[1].data[0] == 0 && r[1].data[1] == 0);

    // ---------------- search after the startup RESET: supported sensors become ONE_WIRE modules
    check("bus RESET: passed on to the wire, asks for a full search",
          send(tfbus_reset(f, sizeof f, TFBUS_BROADCAST)) == 0 && s_requests == 1 && s_replace_requests == 1);
    const uint64_t found[3] = {ds18b20, ds2438, ds18s20};
    localio_sensors_search_done(found, 3, true);
    check("DISCOVER: ANALOG_DIN again after the RESET", discover(&id) && id.hw_type == 0x19);
    send(tfbus_addr_assign(f, sizeof f, 0x19000001, 1));
    check("DISCOVER: DS18B20 as ONE_WIRE 0xff fw 0x28 api 3",
          discover(&id) && id.sn == 0x34567890 && id.hw_type == 0xff && id.fw_type == 0x28 && id.fw_api_ver == 3);
    send(tfbus_addr_assign(f, sizeof f, 0x34567890, 2));
    check("DISCOVER: DS18S20 as ONE_WIRE fw 0x10 api 1 (DS2438 skipped)",
          discover(&id) && id.sn == localio_sensor_serial(ds18s20) && id.fw_type == 0x10 && id.fw_api_ver == 1);
    send(tfbus_addr_assign(f, sizeof f, localio_sensor_serial(ds18s20), 3));
    check("no more modules", send(tfbus_discover(f, sizeof f)) == 0);
    uint64_t listed[LOCALIO_SENSORS_MAX];
    check("list to read: the 2 sensors",
          localio_sensors_list(listed, LOCALIO_SENSORS_MAX) == 2 && listed[0] == ds18b20 && listed[1] == ds18s20);
    r[0] = poll(1);
    tfbus_parse_tlv(pl, plen, r, 2);
    check("hub record: 2 supported found, 1 search", r[1].data[0] == 2 && r[1].data[1] == 1);

    // ---------------- readings
    r[0] = poll(2);
    check("sensor not read yet: state 2, hub SN, ROM",
          r[0].type == 0x23 && r[0].len == 15 && r[0].data[0] == 2 && le16(r[0].data + 3) == 0x0001 &&
              r[0].data[6] == 0x19 && r[0].data[7] == 0x28 && r[0].data[14] == 0x5a);
    localio_sensors_reading(ds18b20, true, 2150);
    r[0] = poll(2);
    check("good read: state 1, 21.50 C", r[0].data[0] == 1 && le16(r[0].data + 1) == 2150);
    localio_sensors_reading(ds18b20, false, 0);
    localio_sensors_reading(ds18b20, false, 0);
    r[0] = poll(2);
    check("2 failed reads: still connected, last value", r[0].data[0] == 1 && le16(r[0].data + 1) == 2150);
    localio_sensors_reading(ds18b20, false, 0);
    r[0] = poll(2);
    check("3rd failed read: disconnected", r[0].data[0] == 0);
    localio_sensors_reading(ds18b20, true, 2200);
    r[0] = poll(2);
    check("answers again: connected", r[0].data[0] == 1 && le16(r[0].data + 1) == 2200);
    localio_sensors_reading(0x1234, true, 0); // not listed: ignored

    // ---------------- RefreshSensors: adds only; OM refresh (RESET): replaces the list
    const tfbus_tlv_t refresh = {.type = 0x22};
    check("MODULE_CMD REFRESH to the hub: ACK + additive search request",
          send(tfbus_module_cmd(f, sizeof f, 1, &refresh, 1)) == 4 && s_requests == 2 && s_replace_requests == 1);
    const uint64_t only_ds18s20[1] = {ds18s20};
    localio_sensors_search_done(only_ds18s20, 1, false);
    check("additive search: the DS18B20 not found is kept",
          localio_sensors_list(listed, LOCALIO_SENSORS_MAX) == 2 && poll(2).data[0] == 1);
    localio_sensors_search_done(only_ds18s20, 1, true);
    check("replacing search: the DS18B20 is dropped, the DS18S20 keeps its address",
          localio_sensors_list(listed, LOCALIO_SENSORS_MAX) == 1 && listed[0] == ds18s20 && poll(3).type == 0x23 &&
              poll(2).type == 0xff);
    const uint64_t both[2] = {ds18s20, ds18b20};
    localio_sensors_search_done(both, 2, false);
    check("sensor back: new module in the reset state, no address yet",
          localio_sensors_list(listed, LOCALIO_SENSORS_MAX) == 2 && discover(&id) && id.sn == 0x34567890);
    send(tfbus_addr_assign(f, sizeof f, 0x34567890, 2));
    check("  state 2 until read", poll(2).data[0] == 2);

    uint64_t many[LOCALIO_SENSORS_MAX + 4];
    for (int i = 0; i < LOCALIO_SENSORS_MAX + 4; i++) {
        many[i] = 0x28 | (uint64_t)(i + 1) << 8;
    }
    localio_sensors_search_done(many, LOCALIO_SENSORS_MAX + 4, true);
    check("at most LOCALIO_SENSORS_MAX sensors", localio_sensors_list(listed, LOCALIO_SENSORS_MAX + 4) == LOCALIO_SENSORS_MAX);
    check("lock released everywhere", host_mutex_held == 0);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
