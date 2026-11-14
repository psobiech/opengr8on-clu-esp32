/**
 * @file clu_state.c
 * @brief CLU identity (serial, MAC, PIN) and project keys in NVS, plus the a:\\ config files OM reads.
 */
#include "clu_state.h"

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#include "clu_keys.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tfbus_master.h"

static const char *TAG = "clu_state";
static const char *NS = "clu";
#define CFG_PART "cfg" // provisioning partition and its namespace (tools/mknvs.py)

clu_state_t g_clu;

/** @brief CONFIG_CLU_IV as 16 bytes; false when empty or malformed. */
static bool config_iv(uint8_t out[16])
{
    const char *configured_iv = CONFIG_CLU_IV;
    if (strlen(configured_iv) != 32) {
        if (configured_iv[0]) {
            ESP_LOGW(TAG, "CLU_IV must be 32 hex chars, using random");
        }
        return false;
    }
    for (int i = 0; i < 16; i++) {
        char hex_pair[3] = {configured_iv[2 * i], configured_iv[2 * i + 1], 0};
        char *end;
        out[i] = (uint8_t)strtoul(hex_pair, &end, 16);
        if (*end) {
            ESP_LOGW(TAG, "CLU_IV not hex, using random");
            return false;
        }
    }
    return true;
}

/**
 * @brief Open the provisioning partition "cfg" (tools/mknvs.py image), read-only.
 *
 * Separate from the runtime NVS: flashing settings never touches project keys, static IP, the
 * TF-Bus registry... Initialised on first use; a missing or blank partition = nothing set.
 */
static bool cfg_open(nvs_handle_t *nvs_handle)
{
    static bool ready; // failures are not cached: only boot-time calls, nothing to save
    if (!ready) {
        esp_err_t err = nvs_flash_init_partition(CFG_PART);
        ready = err == ESP_OK;
        if (!ready) {
            ESP_LOGI(TAG, "no settings partition \"%s\" (%s)", CFG_PART, esp_err_to_name(err));
        }
    }
    return ready && nvs_open_from_partition(CFG_PART, CFG_PART, NVS_READONLY, nvs_handle) == ESP_OK;
}

const char *clu_state_setting(const char *key, const char *fallback)
{
    nvs_handle_t nvs_handle;
    if (!cfg_open(&nvs_handle)) {
        return fallback; // nothing provisioned
    }
    size_t len = 0;
    char *setting_value = NULL;
    if (nvs_get_str(nvs_handle, key, NULL, &len) == ESP_OK && len > 0 && (setting_value = malloc(len)) != NULL &&
        nvs_get_str(nvs_handle, key, setting_value, &len) != ESP_OK) {
        free(setting_value);
        setting_value = NULL;
    }
    nvs_close(nvs_handle);
    if (setting_value) {
        ESP_LOGI(TAG, "setting %s from the cfg partition", key); // never the value: may be a password
    }
    return setting_value ? setting_value : fallback;
}

void clu_state_factory_keys(void)
{
    clu_default_key((const uint8_t *)g_clu.pin, g_clu.project.key);
    if (!config_iv(g_clu.project.iv)) {
        clu_random(g_clu.project.iv, sizeof g_clu.project.iv);
    }
    g_clu.has_temp = false;
}

void clu_state_save_keys(void)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NS, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        return;
    }
    nvs_set_blob(nvs_handle, "key", g_clu.project.key, 16);
    nvs_set_blob(nvs_handle, "iv", g_clu.project.iv, 16);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

void clu_state_load(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_clu.mac, sizeof g_clu.mac, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &nvs_handle));

    // Serial / PIN to adopt: menuconfig, else the provisioning partition (prebuilt firmware).
    uint32_t want_sn = 0;
    char want_pin[CLU_PIN_BYTES + 1] = "";
    if (CONFIG_CLU_SERIAL[0]) {
        want_sn = (uint32_t)strtoul(CONFIG_CLU_SERIAL, NULL, 10);
    }
    if (strlen(CONFIG_CLU_PRIVATE_KEY) == CLU_PIN_BYTES) {
        strlcpy(want_pin, CONFIG_CLU_PRIVATE_KEY, sizeof want_pin);
    }
    nvs_handle_t provisioning_handle;
    if ((!want_sn || !want_pin[0]) && cfg_open(&provisioning_handle)) {
        if (!want_sn && nvs_get_u32(provisioning_handle, "sn", &want_sn) != ESP_OK) {
            want_sn = 0;
        }
        char pin_buffer[16];
        size_t pin_buffer_length = sizeof pin_buffer;
        if (!want_pin[0] && nvs_get_str(provisioning_handle, "pin", pin_buffer, &pin_buffer_length) == ESP_OK && strlen(pin_buffer) == CLU_PIN_BYTES) {
            strlcpy(want_pin, pin_buffer, sizeof want_pin);
        }
        nvs_close(provisioning_handle);
    }

    bool dirty = false;
    bool identity_changed = false;
    uint32_t old_sn = 0;
    bool have_sn = nvs_get_u32(nvs_handle, "sn", &old_sn) == ESP_OK;
    if (want_sn) {
        g_clu.sn = want_sn;
        if (!have_sn || old_sn != g_clu.sn) {
            nvs_set_u32(nvs_handle, "sn", g_clu.sn);
            identity_changed = have_sn;
            dirty = true;
        }
    } else if (have_sn) {
        g_clu.sn = old_sn;
    } else {
        do {
            clu_random((uint8_t *)&g_clu.sn, sizeof g_clu.sn);
            g_clu.sn &= 0x7FFFFFFF;
        } while (g_clu.sn == 0);
        nvs_set_u32(nvs_handle, "sn", g_clu.sn);
        dirty = true;
    }

    size_t len = sizeof g_clu.pin;
    bool have_pin = nvs_get_str(nvs_handle, "pin", g_clu.pin, &len) == ESP_OK;
    if (want_pin[0]) {
        if (!have_pin || strcmp(g_clu.pin, want_pin) != 0) {
            strlcpy(g_clu.pin, want_pin, sizeof g_clu.pin);
            nvs_set_str(nvs_handle, "pin", g_clu.pin);
            identity_changed |= have_pin;
            dirty = true;
        }
    } else if (!have_pin) {
        // 8 chars from A-Z0-9 (~41 bits; was 8 hex digits = 32 bits). The factory key is
        // DEFAULT_IV ^ PIN, so the PIN is the only secret before OM sets a project key.
        static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for (int i = 0; i < CLU_PIN_BYTES; i++) {
            uint8_t random_byte;
            do {
                clu_random(&random_byte, 1);
            } while (random_byte >= 252); // 252 = 7 * 36: no modulo bias
            g_clu.pin[i] = abc[random_byte % 36];
        }
        g_clu.pin[CLU_PIN_BYTES] = 0;
        nvs_set_str(nvs_handle, "pin", g_clu.pin);
        dirty = true;
    }

    // Configured factory IV changed since last boot -> keys back to factory default.
    char civ[33] = "";
    size_t configured_iv_length = sizeof civ;
    nvs_get_str(nvs_handle, "civ", civ, &configured_iv_length);
    if (strcmp(civ, CONFIG_CLU_IV) != 0) {
        nvs_set_str(nvs_handle, "civ", CONFIG_CLU_IV);
        identity_changed |= CONFIG_CLU_IV[0] || civ[0];
        dirty = true;
    }

    size_t project_key_length = 16, project_iv_length = 16;
    if (identity_changed ||
        nvs_get_blob(nvs_handle, "key", g_clu.project.key, &project_key_length) != ESP_OK ||
        nvs_get_blob(nvs_handle, "iv", g_clu.project.iv, &project_iv_length) != ESP_OK) {
        clu_state_factory_keys();
        nvs_set_blob(nvs_handle, "key", g_clu.project.key, 16);
        nvs_set_blob(nvs_handle, "iv", g_clu.project.iv, 16);
        dirty = true;
    }

    if (dirty) {
        nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "SN=%08lx (%lu) MAC=%s PIN(private key)=%s",
             (unsigned long)g_clu.sn, (unsigned long)g_clu.sn, g_clu.mac, g_clu.pin);
}

void clu_state_write_config_files(void)
{
    char mac[18];
    snprintf(mac, sizeof mac, "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s", g_clu.mac, g_clu.mac + 2,
             g_clu.mac + 4, g_clu.mac + 6, g_clu.mac + 8, g_clu.mac + 10);
    for (char *c = mac; *c; c++) {
        if (*c >= 'A' && *c <= 'F') {
            *c += 'a' - 'A';
        }
    }

    // tfbusDevices: modules found by the last bus discovery, in the real CLU's format, e.g.
    // {"sn":252017533,"hwType":3,"hwVer":1,"fwType":3,"fwApiVer":4,"fwVer":"0.0.0","status":"OK"}
    // config.txt below carries the same list in the format OM actually imports.
    static tfbus_device_t dev[TFBUS_MAX_MODULES];
    int ndev = tfbus_master_devices(dev, TFBUS_MAX_MODULES);
    FILE *config_file = fopen(CLU_FS_ROOT "/a/CONFIG.JSON", "w");
    if (config_file) {
        fprintf(config_file,
                "{\"sn\":%lu,\"mac\":\"%s\",\"hwType\":%d,\"hwVer\":%d,\"fwType\":%d,"
                "\"fwApiVer\":%d,\"fwVer\":\"%s\",\"status\":\"OK\",\"tfbusDevices\":[",
                (unsigned long)g_clu.sn, mac, CLU_HW_TYPE, CLU_HW_VERSION, CLU_FW_TYPE,
                CLU_FW_API_VER, CLU_FW_VERSION);
        for (int i = 0; i < ndev; i++) {
            const tfbus_device_t *device = &dev[i];
            fprintf(config_file,
                    "%s{\"sn\":%lu,\"hwType\":%u,\"hwVer\":%u,\"fwType\":%u,\"fwApiVer\":%u,"
                    "\"fwVer\":\"%u.%u.%u\",\"status\":\"OK\"}",
                    i ? "," : "", (unsigned long)device->sn, device->hw_type, device->hw_ver, device->fw_type,
                    device->fw_api_ver, device->have_ver ? device->ver[0] : 0, device->have_ver ? device->ver[1] : 0,
                    device->have_ver ? device->ver[2] : 0);
        }
        fputs("]}", config_file);
        fclose(config_file);
    }
    // LF only, like the real CLU and the vCLU: TFTP netascii turns each LF into CR LF on the
    // wire, so a CR LF in the file would reach OM as CR NUL CR LF.
    config_file = fopen(CLU_FS_ROOT "/a/CONFIG.TXT", "w");
    if (config_file) {
        fprintf(config_file, "%08x\n%08lx\n%s\n%08x\n%08x\n%08x\n%08x\n", 0,
                (unsigned long)g_clu.sn, mac, CLU_FW_TYPE, CLU_FW_API_VER, CLU_HW_TYPE,
                CLU_HW_VERSION);
        // Bus modules, one per line "sn:hwType:fwType:fwApiVer" (hex, real CLU format). This is
        // where OM 1.12.1 reads the module list from, not CONFIG.JSON.
        for (int i = 0; i < ndev; i++) {
            fprintf(config_file, "%08lx:%02x:%02x:%02x\n", (unsigned long)dev[i].sn, dev[i].hw_type,
                    dev[i].fw_type, dev[i].fw_api_ver);
        }
        fclose(config_file);
    }

    // OM's "get configuration" downloads config.txt, CONFIG.JSON and om.lua with no per-file
    // error handling: a missing om.lua aborts the whole transfer. A factory CLU has (empty)
    // placeholders; create them if absent. MAIN.LUA is left out on purpose (absent = idle).
    static const char *const placeholders[] = {CLU_FS_ROOT "/a/OM.LUA", CLU_FS_ROOT "/a/USER.LUA"};
    for (size_t i = 0; i < sizeof placeholders / sizeof placeholders[0]; i++) {
        config_file = fopen(placeholders[i], "r");
        if (config_file) {
            fclose(config_file);
            continue;
        }
        config_file = fopen(placeholders[i], "w");
        if (config_file) {
            fclose(config_file);
        }
    }

    // Grenton cloud enrollment: with UseCloud=1 OM sends the project, then checks
    // a:\CLOUD-PUBLIC.CRT; if it is missing or empty it reads a:\CLOUD-PUBLIC.CSR, posts it to the
    // Grenton cloud and uploads the signed certificate (OM 1.12.1). This CLU never talks to the Grenton cloud (UseCloud drives
    // our own MQTT link instead), so a non-empty placeholder makes OM skip enrollment.
    struct stat file_status;
    if (stat(CLU_FS_ROOT "/a/CLOUD-PUBLIC.CRT", &file_status) != 0 || file_status.st_size == 0) {
        config_file = fopen(CLU_FS_ROOT "/a/CLOUD-PUBLIC.CRT", "w");
        if (config_file) {
            fputs("# OpenGr8on CLU: no Grenton cloud. UseCloud controls the MQTT broker link.\n", config_file);
            fclose(config_file);
        } else {
            ESP_LOGW(TAG, "cannot create a:\\CLOUD-PUBLIC.CRT (FAT long names off?)");
        }
    }
}
