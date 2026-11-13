/**
 * @file clu_diag.c
 * @brief m:\\DIAGNOSTIC_PACK.JSON in the layout of a real CLU_ZWAVE_2 (cJSON formatted, tab indent).
 *
 * OM (GetTFTPDiagnosticDtoRequest, CluDiagnosticDto / TfBusDiagnosticDto) reads: sn, status,
 * ipAddress, cloudConn, voltage, hwType, hwVer, fwType, fwApiVer, fwVer, tfbusDiagnostic[]
 * {sn, status, voltage, fails, banned, hwType, hwVer, fwType, fwApiVer, fwVer}. All optional.
 *
 * "All optional" in the DTO, but OM's Diagnostics view calls getDeviceName(hwType, hwVer, fwType)
 * with unboxed longs for every module it knows: a missing hwVer or
 * fwType there is a NullPointerException and the whole view fails. So every module entry carries
 * hwType/hwVer/fwType/fwApiVer/fwVer, like the real CLU: values from the last discovery
 * (DISCOVER / VERSION_QUERY), else the same defaults as a:\\CONFIG.JSON (hwVer 1, fwType 3 for
 * SMART_PANEL / 2 otherwise, fwVer "0.0.0"). Voltage is not measured: 0, which is what the real CLU
 * writes for modules without voltage reporting (OM shows 0.0 V; a missing key shows "n/a").
 *
 * Module status "OK" / "DISCONNECTED" (Status enum in OM).
 */
#include "clu_diag.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "clu_mqtt.h"
#include "clu_state.h"
#include "esp_log.h"
#include "tfbus_master.h"

static const char *TAG = "diag";

#define DIAG_DIR  CLU_FS_ROOT "/m"
#define DIAG_FILE DIAG_DIR "/DIAGNOSTIC_PACK.JSON"

int clu_diag_write(const char *ip)
{
    if (mkdir(DIAG_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir " DIAG_DIR ": errno %d", errno);
        return 1;
    }
    FILE *f = fopen(DIAG_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "open " DIAG_FILE ": errno %d", errno);
        return 2;
    }
    fprintf(f,
            "{\n\t\"sn\":\t%lu,\n\t\"status\":\t\"OK\",\n\t\"ipAddress\":\t\"%s\",\n"
            "\t\"cloudConn\":\t%s,\n\t\"voltage\":\t0,\n\t\"hwType\":\t%d,\n\t\"hwVer\":\t%d,\n\t\"fwType\":\t%d,\n"
            "\t\"fwApiVer\":\t%d,\n\t\"fwVer\":\t\"%s\",\n\t\"tfbusDiagnostic\":\t[",
            (unsigned long)g_clu.sn, ip, clu_mqtt_connected() ? "true" : "false", CLU_HW_TYPE,
            CLU_HW_VERSION, CLU_FW_TYPE, CLU_FW_API_VER, CLU_FW_VERSION);
    tfbus_device_t *dev = calloc(TFBUS_MAX_MODULES, sizeof *dev);
    int ndev = dev ? tfbus_master_devices(dev, TFBUS_MAX_MODULES) : 0;
    int n = tfbus_master_count(), out = 0;
    for (int i = 0; i < n; i++) {
        const tfbus_module_t *m = tfbus_master_get(i);
        if (!m) {
            continue; // slot freed by req_refresh_modules
        }
        const tfbus_device_t *d = NULL;
        for (int j = 0; j < ndev && !d; j++) {
            d = dev[j].sn == m->sn ? &dev[j] : NULL;
        }
        unsigned hw_ver = d && d->hw_ver ? d->hw_ver : 1;
        unsigned fw_type = d && d->fw_type ? d->fw_type : (m->hw_type == 0x03 ? 3 : 2);
        unsigned fw_api = d ? d->fw_api_ver : 0;
        bool v = d && d->have_ver;
        fprintf(f,
                "%s{\n\t\t\t\"sn\":\t%lu,\n\t\t\t\"status\":\t\"%s\",\n\t\t\t\"voltage\":\t0,\n\t\t\t\"fails\":\t%lu,\n"
                "\t\t\t\"banned\":\tfalse,\n\t\t\t\"hwType\":\t%d,\n\t\t\t\"hwVer\":\t%u,\n"
                "\t\t\t\"fwType\":\t%u,\n\t\t\t\"fwApiVer\":\t%u,\n\t\t\t\"fwVer\":\t\"%u.%u.%u\"\n\t\t}",
                out++ ? ", " : "", (unsigned long)m->sn, m->online ? "OK" : "DISCONNECTED",
                (unsigned long)m->fails_total, m->hw_type, hw_ver, fw_type, fw_api,
                v ? d->ver[0] : 0, v ? d->ver[1] : 0, v ? d->ver[2] : 0);
    }
    free(dev);
    fprintf(f, "]\n}");
    bool ok = !ferror(f);
    if (fclose(f) != 0 || !ok) {
        ESP_LOGE(TAG, "write " DIAG_FILE " failed");
        return 2;
    }
    ESP_LOGI(TAG, "diagnostic pack written (%d modules)", n);
    return 0;
}
