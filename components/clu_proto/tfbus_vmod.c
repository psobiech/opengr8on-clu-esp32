/**
 * @file tfbus_vmod.c
 * @brief Module side of TF-Bus: emulated modules (see tfbus_vmod.h).
 */
#include "tfbus_vmod.h"

#include <string.h>

#include "tfbus_frame.h"

/** @brief Module -> CLU frame AA 00 len payload XOR (len = payload + 1); 0 if it does not fit. */
static int reply(uint8_t *rx, size_t cap, const uint8_t *reply_payload, size_t payload_length)
{
    if (payload_length + 4 > cap) {
        return 0;
    }
    rx[0] = TFBUS_START;
    rx[1] = TFBUS_CLU_ADDR;
    rx[2] = (uint8_t)(payload_length + 1);
    if (payload_length) {
        memcpy(rx + 3, reply_payload, payload_length);
    }
    rx[3 + payload_length] = tfbus_xor(rx, 3 + payload_length);
    return (int)(4 + payload_length);
}

static int broadcast(tfbus_vmod_t *modules, int module_count, uint8_t type, const uint8_t *command_payload, size_t plen, uint8_t *rx,
                     size_t rxcap)
{
    switch (type) {
    case TFBUS_T_RESET:
        for (int i = 0; i < module_count; i++) {
            modules[i].addr = 0;
        }
        return 0; // modules on the wire must hear it too
    case TFBUS_T_DISCOVER:
        for (int i = 0; i < module_count; i++) {
            if (!modules[i].addr) {
                uint32_t serial_number = modules[i].sn;
                const uint8_t discovery_payload[7] = {(uint8_t)serial_number, (uint8_t)(serial_number >> 8), (uint8_t)(serial_number >> 16), (uint8_t)(serial_number >> 24),
                                      modules[i].ops->hw_type, modules[i].ops->fw_type, modules[i].ops->fw_api_ver};
                return reply(rx, rxcap, discovery_payload, sizeof discovery_payload);
            }
        }
        return 0; // all addressed: the next DISCOVER is for the wire
    case TFBUS_T_ADDR_ASSIGN:
        if (plen >= 5) {
            uint32_t serial_number = command_payload[0] | command_payload[1] << 8 | command_payload[2] << 16 | (uint32_t)command_payload[3] << 24;
            for (int i = 0; i < module_count; i++) {
                if (modules[i].sn == serial_number) {
                    modules[i].addr = command_payload[4];
                    return reply(rx, rxcap, NULL, 0);
                }
            }
        }
        return 0;
    default:
        return 0; // SEEK etc.: for the wire
    }
}

int tfbus_vmod_frame(tfbus_vmod_t *modules, int module_count, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxcap)
{
    if (!modules || module_count <= 0 || txlen < 4 || tx[0] != TFBUS_START || tfbus_xor(tx, txlen) != 0) {
        return 0;
    }
    uint8_t dest = tx[1], type = tx[2];
    const uint8_t *command_payload = tx + 3;
    size_t plen = txlen - 4;
    if (dest == TFBUS_BROADCAST) {
        return broadcast(modules, module_count, type, command_payload, plen, rx, rxcap);
    }
    tfbus_vmod_t *mod = NULL;
    for (int i = 0; i < module_count && !mod; i++) {
        if (modules[i].addr && modules[i].addr == dest) {
            mod = &modules[i];
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
        uint8_t status_payload[TFBUS_MAX_PAYLOAD];
        int len = mod->ops->status(mod, status_payload, sizeof status_payload);
        return len < 0 ? -1 : reply(rx, rxcap, status_payload, (size_t)len);
    }
    case TFBUS_T_VERSION_QUERY:
        return reply(rx, rxcap, mod->ops->version, sizeof mod->ops->version);
    default:
        if (mod->ops->command && !mod->ops->command(mod, type, command_payload, plen)) {
            return -1;
        }
        return reply(rx, rxcap, NULL, 0);
    }
}

void tfbus_din8_status(uint8_t inputs, uint8_t outputs, uint8_t status_bytes[TFBUS_DIN8_STATUS_LEN])
{
    const uint8_t status_template[TFBUS_DIN8_STATUS_LEN] = {2, 0x00, 1, 1, inputs, 0x01, 1, 1, outputs};
    memcpy(status_bytes, status_template, sizeof status_template);
}

bool tfbus_din8_cmd(const uint8_t *command_payload, size_t plen, uint8_t *out)
{
    tfbus_tlv_t tlv_records[8];
    // Payload: plen count {type len meta data}...; tfbus_parse_tlv() starts at the count.
    int record_count = plen >= 2 ? tfbus_parse_tlv(command_payload + 1, plen - 1, tlv_records, 8) : -1;
    if (record_count < 0) {
        return false;
    }
    for (int k = 0; k < record_count && k < 8; k++) {
        if ((tlv_records[k].type == 0x08 || tlv_records[k].type == 0x09) && tlv_records[k].len >= 1) {
            *out = tlv_records[k].type == 0x08 ? (uint8_t)(*out | tlv_records[k].data[0]) : (uint8_t)(*out & ~tlv_records[k].data[0]);
        }
    }
    return true;
}
