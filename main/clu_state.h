/**
 * @file clu_state.h
 * @brief Persistent CLU identity and keys (NVS). Port of src/vclu/Config.ts.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "clu_crypto.h"

/**
 * @name Identity reported to OM
 *
 * Real CLU_ZWAVE_2 (diagnostic pack of a production CLU: hwType 19, hwVer 1, fwType 3,
 * fwApiVer 513, fwVer "5.13.8-2442"). Reported by default: latest firmware 5.15.24 (build 2635),
 * fwApiVer 0x203 (515), fwVer "5.15.24-2635". OM selects
 * `docs/device-interfaces/clu_ZWAVE_2_ft00000003_fv<fwApiVer>_ht00000013_hv00000001.xml`
 * and generates OM.LUA with `-- FwVersion <fwApiVer>`.
 * @{
 */
#define CLU_HW_TYPE    0x13
#define CLU_HW_VERSION 0x01
#define CLU_FW_TYPE    0x03
#define CLU_FW_API_VER CONFIG_CLU_FW_API_VER
/** @} */

#include "sdkconfig.h"

/** @brief VFS mount point holding the drive directories (a, p, ...). */
#define CLU_FS_ROOT "/fs"

/** @brief CLU identity and current cipher keys. */
typedef struct {
    uint32_t     sn;           /**< Serial number. */
    char         mac[13];      /**< 12 hex chars, uppercase, no separators. */
    char         pin[9];       /**< 8 char private key (ASCII), shown to user for OM. */
    clu_cipher_t project;      /**< Current project key + IV. */
    bool         has_temp;     /**< `temp` is valid. */
    clu_cipher_t temp;         /**< Project key with temporary IV (after discovery). */
} clu_state_t;

/** @brief Global CLU state. */
extern clu_state_t g_clu;

/**
 * @brief Load the state from NVS or generate it.
 *
 * sn/pin/iv are random unless CLU_SERIAL/CLU_PRIVATE_KEY/CLU_IV are set, or sn/pin are in the
 * provisioning partition "cfg" (menuconfig wins). An adopted serial / PIN that differs from the
 * stored one resets the project keys to the factory key; project key = default_key(pin).
 */
void clu_state_load(void);

/** @brief Persist the project key and IV to NVS. */
void clu_state_save_keys(void);

/** @brief Wipe keys back to factory default (default_key(pin), IV = CLU_IV or random). */
void clu_state_factory_keys(void);

/**
 * @brief Write a:\\CONFIG.JSON and a:\\config.txt (port of vclu/Config.ts writeDeviceConfig).
 *
 * Both list the modules found by the last bus discovery. Also creates the empty OM.LUA / USER.LUA
 * placeholders and a CLOUD-PUBLIC.CRT placeholder (skips Grenton cloud enrollment in OM).
 */
void clu_state_write_config_files(void);

/**
 * @brief Device setting from the provisioning partition "cfg" (string key), for prebuilt firmware.
 *
 * Keys: wifi_ssid, wifi_pass, mqtt_uri, mqtt_user, mqtt_pass (see tools/mknvs.py; the same
 * partition also carries sn / pin, adopted by clu_state_load()). A value present there wins over
 * the menuconfig default. The partition is separate from the runtime NVS; the firmware only reads
 * it (NVS formats it once if blank).
 *
 * @param key      NVS key.
 * @param fallback Returned when the key is not set (may be NULL).
 * @return The stored value (heap copy, kept for the program's lifetime) or @p fallback.
 */
const char *clu_state_setting(const char *key, const char *fallback);
