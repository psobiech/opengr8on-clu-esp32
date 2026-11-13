/**
 * @file clu_ota.h
 * @brief Password-protected firmware update over HTTP (POST /ota) and rollback confirmation.
 *
 * The application image (release asset opengr8on-clu-<board>-<version>-ota.bin = the build's
 * opengr8on_clu_esp32.bin, not the merged full image) is streamed into the
 * next OTA slot, verified by ESP-IDF (header, chip, SHA-256), checked to be this project's
 * firmware, made the boot partition, and the CLU restarts:
 *
 *     curl -u ota:PASSWORD --data-binary @opengr8on-clu-gate-v1.0.0-ota.bin http://CLU_IP:8080/ota
 *
 * GET / serves an upload page (same origin, any browser). With a CORS origin set, a page from that
 * origin (the hosted web flasher) may call POST /ota too, as far as the browser lets an HTTPS page
 * reach a LAN HTTP address (Chrome: Local Network Access permission).
 *
 * HTTP Basic auth: the user name is ignored, the password must match. A wrong password costs
 * ~2 s (the server handles one request at a time). Plain HTTP: the password crosses the LAN in
 * clear text.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** @brief OTA server configuration. */
typedef struct {
    uint16_t port;        /**< TCP port of the HTTP server. */
    const char *password; /**< 8..64 characters; NULL / "" = OTA off (server not started). */
    const char *cors_origin; /**< Origin allowed to POST /ota from a browser (CORS), NULL / "" = none. */
} clu_ota_cfg_t;

/**
 * @brief Start the HTTP server with POST /ota (after esp_netif_init()).
 *
 * @param cfg Configuration (the password is copied).
 * @return true when OTA updates are on; false without a (valid) password or when the server
 *         cannot start.
 */
bool clu_ota_start(const clu_ota_cfg_t *cfg);

/**
 * @brief Mark the running firmware valid if it is a fresh OTA image still pending verification
 * (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): call once the network is up. An image that restarts
 * before that is rolled back by the bootloader.
 */
void clu_ota_confirm(void);
