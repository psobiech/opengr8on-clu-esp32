/**
 * @file main.c
 * @brief Firmware entry point: OpenGr8on ESP32-S3 CLU.
 *
 * Boot order: NVS, crypto, CLU identity, telnet log hook, time zone, FAT storage (a:\\, p:\\),
 * config files, RS485 + TFBUS master and drivers, WiFi (wait for IP), SNTP, telnet server, MQTT,
 * Lua runtime, TFTP and the UDP/1234 CLU protocol server.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "clu_crypto.h"
#include "clu_lua.h"
#include "clu_mqtt.h"
#include "clu_net.h"
#include "clu_time.h"
#include "clu_state.h"
#include "clu_sysinfo.h"
#include "clu_telnet.h"
#include "clu_tftp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "tfbus_drivers.h"
#include "tfbus_master.h"
#include "tfbus_uart.h"
#include "udp_server.h"

static const char *TAG = "main";
static EventGroupHandle_t s_wifi_ev;
#define WIFI_UP BIT0

/**
 * @brief WiFi / IP event handler: scan-then-connect, reconnect on disconnect, publish the IP.
 */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Debug: one scan first to log matching APs (security, channel), then connect.
        wifi_scan_config_t sc = {.ssid = (uint8_t *)CONFIG_CLU_WIFI_SSID, .show_hidden = true};
        if (esp_wifi_scan_start(&sc, false) != ESP_OK) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = 8;
        wifi_ap_record_t ap[8];
        esp_wifi_scan_get_ap_records(&n, ap);
        ESP_LOGW(TAG, "scan: %u AP(s) named [%s]", n, CONFIG_CLU_WIFI_SSID);
        for (int i = 0; i < n; i++) {
            // authmode: 3=WPA2_PSK 6=WPA3_PSK 7=WPA2_WPA3_PSK; cipher 3=CCMP
            ESP_LOGW(TAG, "  " MACSTR " ch %d rssi %d authmode %d pairwise %d group %d 11b%d g%d n%d ax%d",
                     MAC2STR(ap[i].bssid), ap[i].primary, ap[i].rssi, ap[i].authmode,
                     ap[i].pairwise_cipher, ap[i].group_cipher, ap[i].phy_11b, ap[i].phy_11g,
                     ap[i].phy_11n, ap[i].phy_11ax);
        }
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *e = data;
        ESP_LOGI(TAG, "WiFi connected to \"%.*s\" ch %d authmode %d",
                 e->ssid_len, (const char *)e->ssid, e->channel, e->authmode);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;
        // 2=AUTH_EXPIRE 15=4WAY_HANDSHAKE_TIMEOUT 201=NO_AP_FOUND 202=AUTH_FAIL
        // 204=HANDSHAKE_TIMEOUT 205=CONNECTION_FAIL 210=NO_AP_FOUND_W_COMPATIBLE_SECURITY
        // 211=NO_AP_FOUND_IN_AUTHMODE_THRESHOLD 212=NO_AP_FOUND_IN_RSSI_THRESHOLD
        ESP_LOGW(TAG, "WiFi disconnected: reason %d rssi %d", e->reason, e->rssi);
        xEventGroupClearBits(s_wifi_ev, WIFI_UP);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        udp_server_set_local_ip(e->ip_info.ip.addr);
        ESP_LOGI(TAG, "IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_ev, WIFI_UP);
    }
}

/**
 * @brief Initialise netif + WiFi STA from menuconfig (WPA2-PSK minimum, optional BSSID lock) and start it.
 */
static void wifi_start(void)
{
    s_wifi_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    clu_net_init(esp_netif_create_default_wifi_sta());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_CLU_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_CLU_WIFI_PASSWORD, sizeof wc.sta.password);
    // Never join open/WEP/WPA networks (a spoofed AP with the same SSID could otherwise
    // downgrade us); WPA2-PSK minimum, or WPA3-SAE only.
#if CONFIG_CLU_WIFI_WPA3_ONLY
    wc.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;
#else
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
#endif
    // SAE: accept both hunt-and-peck and hash-to-element (H2E-only APs refuse HnP).
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.pmf_cfg.capable = true;
    // Optional fixed AP (e.g. pin one AP of a mesh / avoid band steering).
    if (CONFIG_CLU_WIFI_BSSID[0]) {
        unsigned b[6];
        if (sscanf(CONFIG_CLU_WIFI_BSSID, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            for (int i = 0; i < 6; i++) wc.sta.bssid[i] = (uint8_t)b[i];
            wc.sta.bssid_set = true;
            ESP_LOGW(TAG, "WiFi BSSID locked to " MACSTR, MAC2STR(wc.sta.bssid));
        } else {
            ESP_LOGE(TAG, "CLU_WIFI_BSSID \"%s\" is not aa:bb:cc:dd:ee:ff, ignored", CONFIG_CLU_WIFI_BSSID);
        }
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    // Debug: what the driver actually holds ([] show stray spaces). Remove once WiFi works.
    wifi_config_t got = {0};
    esp_wifi_get_config(WIFI_IF_STA, &got);
    // Password length only: logs also go out over the telnet console.
    ESP_LOGW(TAG, "WiFi cfg: ssid [%s] (%u) pass (%u chars) min_auth %d pwe %d",
             (char *)got.sta.ssid, (unsigned)strlen((char *)got.sta.ssid),
             (unsigned)strlen((char *)got.sta.password),
             got.sta.threshold.authmode, got.sta.sae_pwe_h2e);
    // CLU traffic is latency-sensitive (OM polls with ~4 s timeouts); keep radio awake.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
#if CONFIG_CLU_WIFI_MAX_TX_POWER > 0
    // Some boards' antenna matching fails at full power: RX works, the AP never hears us.
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_CLU_WIFI_MAX_TX_POWER));
    int8_t txp = 0;
    esp_wifi_get_max_tx_power(&txp);
    ESP_LOGW(TAG, "WiFi max TX power %d (x0.25 dBm)", txp);
#endif
}

/**
 * @brief Mount the FAT volume holding the CLU drives.
 *
 * One FAT volume at /fs: /fs/a = a:\\ (MAIN.LUA, OM.LUA, USER.LUA, EMERGNCY.LUA, CONFIG.JSON,
 * SETTINGS.USR), /fs/p = p:\\ (meas.bin).
 */
static void storage_mount(void)
{
    static wl_handle_t wl;
    const esp_vfs_fat_mount_config_t mc = {
        .format_if_mount_failed = true,
        .max_files = 6,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(CLU_FS_ROOT, "storage", &mc, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage mount failed: %s", esp_err_to_name(err));
        return;
    }
    mkdir(CLU_FS_ROOT "/a", 0775);
    mkdir(CLU_FS_ROOT "/p", 0775);
    ESP_LOGI(TAG, "storage mounted at " CLU_FS_ROOT " (a:\\, p:\\)");
}

/**
 * @brief clu_lua_cfg_t::clu_feature: local CLU features applied by the firmware.
 *
 * Values are also kept in the Lua object. Handles 18 UseCloud, 22 NTPServer, 23 TimeZone,
 * 25 PrimaryDNS, 26 SecondaryDNS.
 */
static void on_clu_feature(int idx, const char *str, double num)
{
    switch (idx) {
    case 22: clu_time_set_ntp_server(str); break;       // NTPServer
    case 23: // TimeZone (enum index); NaN / out-of-range doubles must not reach the int cast
        clu_time_set_zone(num >= 0 && num < 1000 ? (int)num : -1);
        break;
    case 25: clu_net_set_dns(0, str); break;            // PrimaryDNS
    case 26: clu_net_set_dns(1, str); break;            // SecondaryDNS
    case 18: clu_mqtt_set_enabled(num != 0); break;     // UseCloud: MQTT connection on/off
    default: break;
    }
}

/**
 * @brief clu_lua_cfg_t::on_reload: forget the bus modules so the next OM.LUA declares them again.
 *
 * Allows for a full discovery (RESET settle, ADDR_ASSIGN retries, first round) still in progress.
 */
static bool on_lua_reload(void)
{
    return tfbus_master_reset(8000);
}

/**
 * @brief clu_lua_cfg_t::on_init (SYSTEM.Init): OM.LUA has declared everything -> start bus and MQTT.
 */
static void on_lua_init(void)
{
    tfbus_master_start();
    clu_mqtt_start();
}

/** @brief ESP-IDF entry point: bring up all subsystems in dependency order. */
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(clu_crypto_init());

    clu_state_load(); // logs the PIN: keep it before the telnet hook so it never leaves UART
#if CONFIG_CLU_TELNET
    const clu_telnet_cfg_t tc = {
        .port = CONFIG_CLU_TELNET_PORT,
        .max_clients = CONFIG_CLU_TELNET_MAX_CLIENTS,
        .history = CONFIG_CLU_TELNET_HISTORY,
        .allow = CONFIG_CLU_TELNET_ALLOW,
    };
    clu_telnet_init(&tc);
#endif
    clu_time_init();
    storage_mount();
    clu_state_write_config_files();

    const tfbus_uart_cfg_t uc = {
        .uart_num = CONFIG_CLU_RS485_UART,
        .tx_pin = CONFIG_CLU_RS485_TX_GPIO,
        .rx_pin = CONFIG_CLU_RS485_RX_GPIO,
        .de_pin = CONFIG_CLU_RS485_DE_GPIO,
        .baud = CONFIG_CLU_RS485_BAUD,
    };
    tfbus_uart_init(&uc);
    tfbus_drivers_register_all();
    tfbus_master_init();
    tfbus_master_on_scan(clu_state_write_config_files); // CONFIG.JSON tfbusDevices

    wifi_start();
    xEventGroupWaitBits(s_wifi_ev, WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);
    clu_time_start_sntp();
#if CONFIG_CLU_TELNET
    clu_telnet_start();
#endif
    const clu_mqtt_cfg_t mc = {
        .sn = g_clu.sn,
        .fw_version = CONFIG_CLU_FW_VERSION,
        .ca_path = CLU_FS_ROOT "/a/MQTT_CA.PEM",
    };
    clu_mqtt_init(&mc);
    clu_sysinfo_init();
    const clu_lua_cfg_t lc = {
        .root = CLU_FS_ROOT "/a",
        .sn = g_clu.sn,
        .fw_version = CONFIG_CLU_FW_VERSION,
        .report = udp_server_send_report,
        .on_init = on_lua_init,
        .remote = udp_server_remote_exec,
        .clu_feature = on_clu_feature,
        .cloud_connected = clu_mqtt_connected,
        .on_reload = on_lua_reload,
    };
    clu_lua_start(&lc);

    clu_tftp_init(CLU_FS_ROOT);
    udp_server_start();
}
