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
#include "tfbus_master.h"

static const char *TAG = "clu_state";
static const char *NS = "clu";

clu_state_t g_clu;

/** @brief CONFIG_CLU_IV as 16 bytes; false when empty or malformed. */
static bool config_iv(uint8_t out[16])
{
    const char *s = CONFIG_CLU_IV;
    if (strlen(s) != 32) {
        if (s[0]) {
            ESP_LOGW(TAG, "CLU_IV must be 32 hex chars, using random");
        }
        return false;
    }
    for (int i = 0; i < 16; i++) {
        char b[3] = {s[2 * i], s[2 * i + 1], 0};
        char *end;
        out[i] = (uint8_t)strtoul(b, &end, 16);
        if (*end) {
            ESP_LOGW(TAG, "CLU_IV not hex, using random");
            return false;
        }
    }
    return true;
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
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "key", g_clu.project.key, 16);
    nvs_set_blob(h, "iv", g_clu.project.iv, 16);
    nvs_commit(h);
    nvs_close(h);
}

void clu_state_load(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_clu.mac, sizeof g_clu.mac, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));

    bool dirty = false;
    bool identity_changed = false;
    uint32_t old_sn = 0;
    bool have_sn = nvs_get_u32(h, "sn", &old_sn) == ESP_OK;
    if (CONFIG_CLU_SERIAL[0]) {
        g_clu.sn = (uint32_t)strtoul(CONFIG_CLU_SERIAL, NULL, 10);
        if (!have_sn || old_sn != g_clu.sn) {
            nvs_set_u32(h, "sn", g_clu.sn);
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
        nvs_set_u32(h, "sn", g_clu.sn);
        dirty = true;
    }

    size_t len = sizeof g_clu.pin;
    bool have_pin = nvs_get_str(h, "pin", g_clu.pin, &len) == ESP_OK;
    if (strlen(CONFIG_CLU_PRIVATE_KEY) == CLU_PIN_BYTES) {
        if (!have_pin || strcmp(g_clu.pin, CONFIG_CLU_PRIVATE_KEY) != 0) {
            strlcpy(g_clu.pin, CONFIG_CLU_PRIVATE_KEY, sizeof g_clu.pin);
            nvs_set_str(h, "pin", g_clu.pin);
            identity_changed |= have_pin;
            dirty = true;
        }
    } else if (!have_pin) {
        // 8 chars from A-Z0-9 (~41 bits; was 8 hex digits = 32 bits). The factory key is
        // DEFAULT_IV ^ PIN, so the PIN is the only secret before OM sets a project key.
        static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for (int i = 0; i < CLU_PIN_BYTES; i++) {
            uint8_t r;
            do {
                clu_random(&r, 1);
            } while (r >= 252); // 252 = 7 * 36: no modulo bias
            g_clu.pin[i] = abc[r % 36];
        }
        g_clu.pin[CLU_PIN_BYTES] = 0;
        nvs_set_str(h, "pin", g_clu.pin);
        dirty = true;
    }

    // Configured factory IV changed since last boot -> keys back to factory default.
    char civ[33] = "";
    size_t cl = sizeof civ;
    nvs_get_str(h, "civ", civ, &cl);
    if (strcmp(civ, CONFIG_CLU_IV) != 0) {
        nvs_set_str(h, "civ", CONFIG_CLU_IV);
        identity_changed |= CONFIG_CLU_IV[0] || civ[0];
        dirty = true;
    }

    size_t kl = 16, il = 16;
    if (identity_changed ||
        nvs_get_blob(h, "key", g_clu.project.key, &kl) != ESP_OK ||
        nvs_get_blob(h, "iv", g_clu.project.iv, &il) != ESP_OK) {
        clu_state_factory_keys();
        nvs_set_blob(h, "key", g_clu.project.key, 16);
        nvs_set_blob(h, "iv", g_clu.project.iv, 16);
        dirty = true;
    }

    if (dirty) {
        nvs_commit(h);
    }
    nvs_close(h);

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
    FILE *f = fopen(CLU_FS_ROOT "/a/CONFIG.JSON", "w");
    if (f) {
        fprintf(f,
                "{\"sn\":%lu,\"mac\":\"%s\",\"hwType\":%d,\"hwVer\":%d,\"fwType\":%d,"
                "\"fwApiVer\":%d,\"fwVer\":\"%s\",\"status\":\"OK\",\"tfbusDevices\":[",
                (unsigned long)g_clu.sn, mac, CLU_HW_TYPE, CLU_HW_VERSION, CLU_FW_TYPE,
                CLU_FW_API_VER, CONFIG_CLU_FW_VERSION);
        for (int i = 0; i < ndev; i++) {
            const tfbus_device_t *d = &dev[i];
            fprintf(f,
                    "%s{\"sn\":%lu,\"hwType\":%u,\"hwVer\":%u,\"fwType\":%u,\"fwApiVer\":%u,"
                    "\"fwVer\":\"%u.%u.%u\",\"status\":\"OK\"}",
                    i ? "," : "", (unsigned long)d->sn, d->hw_type, d->hw_ver, d->fw_type,
                    d->fw_api_ver, d->have_ver ? d->ver[0] : 0, d->have_ver ? d->ver[1] : 0,
                    d->have_ver ? d->ver[2] : 0);
        }
        fputs("]}", f);
        fclose(f);
    }
    // LF only, like the real CLU and the vCLU: TFTP netascii turns each LF into CR LF on the
    // wire, so a CR LF in the file would reach OM as CR NUL CR LF.
    f = fopen(CLU_FS_ROOT "/a/CONFIG.TXT", "w");
    if (f) {
        fprintf(f, "%08x\n%08lx\n%s\n%08x\n%08x\n%08x\n%08x\n", 0,
                (unsigned long)g_clu.sn, mac, CLU_FW_TYPE, CLU_FW_API_VER, CLU_HW_TYPE,
                CLU_HW_VERSION);
        // Bus modules, one per line "sn:hwType:fwType:fwApiVer" (hex, real CLU format). This is
        // where OM 1.12.1 reads the module list from, not CONFIG.JSON.
        for (int i = 0; i < ndev; i++) {
            fprintf(f, "%08lx:%02x:%02x:%02x\n", (unsigned long)dev[i].sn, dev[i].hw_type,
                    dev[i].fw_type, dev[i].fw_api_ver);
        }
        fclose(f);
    }

    // OM's "get configuration" downloads config.txt, CONFIG.JSON and om.lua with no per-file
    // error handling: a missing om.lua aborts the whole transfer. A factory CLU has (empty)
    // placeholders; create them if absent. MAIN.LUA is left out on purpose (absent = idle).
    static const char *const placeholders[] = {CLU_FS_ROOT "/a/OM.LUA", CLU_FS_ROOT "/a/USER.LUA"};
    for (size_t i = 0; i < sizeof placeholders / sizeof placeholders[0]; i++) {
        f = fopen(placeholders[i], "r");
        if (f) {
            fclose(f);
            continue;
        }
        f = fopen(placeholders[i], "w");
        if (f) {
            fclose(f);
        }
    }

    // Grenton cloud enrollment: with UseCloud=1 OM sends the project, then checks
    // a:\CLOUD-PUBLIC.CRT; if it is missing or empty it reads a:\CLOUD-PUBLIC.CSR, posts it to the
    // Grenton cloud and uploads the signed certificate (OM 1.12.1). This CLU never talks to the Grenton cloud (UseCloud drives
    // our own MQTT link instead), so a non-empty placeholder makes OM skip enrollment.
    struct stat st;
    if (stat(CLU_FS_ROOT "/a/CLOUD-PUBLIC.CRT", &st) != 0 || st.st_size == 0) {
        f = fopen(CLU_FS_ROOT "/a/CLOUD-PUBLIC.CRT", "w");
        if (f) {
            fputs("# OpenGr8on CLU: no Grenton cloud. UseCloud controls the MQTT broker link.\n", f);
            fclose(f);
        } else {
            ESP_LOGW(TAG, "cannot create a:\\CLOUD-PUBLIC.CRT (FAT long names off?)");
        }
    }
}
