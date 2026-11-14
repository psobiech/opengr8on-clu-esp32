/**
 * @file main.c
 * @brief Firmware entry point: OpenGr8on ESP32 / ESP32-S3 CLU.
 *
 * Boot order: NVS, crypto, CLU identity, telnet log hook, time zone + clock from the RTC (CLU_RTC), FAT
 * storage (a:\\, p:\\),
 * config files, RS485 + TFBUS master and drivers (CLU_TFBUS) with the on-board IO modules (CLU_LOCAL_IO)
 * or the MySensors gateway (CLU_MYSENSORS) on the same pins, network (WiFi and / or Ethernet), SNTP,
 * telnet server, OTA updates (HTTP), MQTT, TFTP, the UDP/1234 CLU protocol server and the Lua runtime.
 *
 * Nothing waits for an IP: the bus and the local scripts run without the network. Network
 * services (SNTP, MQTT, telnet, UDP) bind to INADDR_ANY / retry on their own and pick the IP up
 * when an interface connects.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "clu_crypto.h"
#include "clu_eth.h"
#if CONFIG_CLU_HTTP
#include "clu_http.h"
#endif
#include "clu_lua.h"
#if CONFIG_CLU_LOCAL_IO
#include "clu_localio.h"
#endif
#include "clu_mqtt.h"
#if CONFIG_CLU_MYSENSORS
#include "clu_mysensors.h"
#endif
#include "clu_ota.h"
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
#include "esp_system.h"
#include "esp_vfs_fat.h"
#if CONFIG_CLU_WIFI
#include "esp_wifi.h"
#endif
#include "freertos/FreeRTOS.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "tfbus_drivers.h"
#include "tfbus_master.h"
#include "tfbus_uart.h"
#include "udp_server.h"

static const char *TAG = "main";

#if !CONFIG_CLU_WIFI && !CONFIG_CLU_ETH
#error "Enable WiFi (CLU_WIFI) and / or Ethernet (CLU_ETH)"
#endif

/**
 * @brief IP event handler (WiFi STA / Ethernet got an address): arm the setup window, poll SNTP, start MQTT.
 */
static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ip_event = data;
    ESP_LOGI(TAG, "%s IP " IPSTR, id == IP_EVENT_STA_GOT_IP ? "WiFi" : "Ethernet", IP2STR(&ip_event->ip_info.ip));
    udp_server_net_up();
    esp_sntp_restart(); // first poll now instead of at the next retry after a failed early one
    clu_mqtt_start(); // MQTT does not wait for the Lua scripts (SYSTEM.Init)
    clu_ota_confirm(); // a fresh OTA image that reached the network is kept (no rollback)
}

#if CONFIG_CLU_WIFI
// SSID / password: cfg partition wifi_ssid / wifi_pass (prebuilt firmware), else menuconfig.
static const char *s_wifi_ssid = CONFIG_CLU_WIFI_SSID;

/**
 * @brief WiFi event handler: scan-then-connect, reconnect on disconnect.
 */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Debug: one scan first to log matching APs (security, channel), then connect.
        wifi_scan_config_t scan_config = {.ssid = (uint8_t *)s_wifi_ssid, .show_hidden = true};
        if (esp_wifi_scan_start(&scan_config, false) != ESP_OK) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t access_point_count = 8;
        wifi_ap_record_t access_points[8];
        esp_wifi_scan_get_ap_records(&access_point_count, access_points);
        ESP_LOGW(TAG, "scan: %u AP(s) named [%s]", access_point_count, s_wifi_ssid);
        for (int i = 0; i < access_point_count; i++) {
            // authmode: 3=WPA2_PSK 6=WPA3_PSK 7=WPA2_WPA3_PSK; cipher 3=CCMP
            ESP_LOGW(TAG, "  " MACSTR " ch %d rssi %d authmode %d pairwise %d group %d 11b%d g%d n%d ax%d",
                     MAC2STR(access_points[i].bssid), access_points[i].primary, access_points[i].rssi, access_points[i].authmode,
                     access_points[i].pairwise_cipher, access_points[i].group_cipher, access_points[i].phy_11b, access_points[i].phy_11g,
                     access_points[i].phy_11n, access_points[i].phy_11ax);
        }
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *wifi_event = data;
        ESP_LOGI(TAG, "WiFi connected to \"%.*s\" ch %d authmode %d",
                 wifi_event->ssid_len, (const char *)wifi_event->ssid, wifi_event->channel, wifi_event->authmode);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *wifi_event = data;
        // 2=AUTH_EXPIRE 15=4WAY_HANDSHAKE_TIMEOUT 201=NO_AP_FOUND 202=AUTH_FAIL
        // 204=HANDSHAKE_TIMEOUT 205=CONNECTION_FAIL 210=NO_AP_FOUND_W_COMPATIBLE_SECURITY
        // 211=NO_AP_FOUND_IN_AUTHMODE_THRESHOLD 212=NO_AP_FOUND_IN_RSSI_THRESHOLD
        ESP_LOGW(TAG, "WiFi disconnected: reason %d rssi %d", wifi_event->reason, wifi_event->rssi);
        esp_wifi_connect();
    }
}

/**
 * @brief Initialise the WiFi STA netif + driver (SSID / password from the cfg partition or menuconfig, WPA2-PSK
 * minimum, optional BSSID lock) and start it. Without an SSID WiFi stays off.
 */
static void wifi_start(void)
{
    s_wifi_ssid = clu_state_setting("wifi_ssid", CONFIG_CLU_WIFI_SSID);
    const char *pass = clu_state_setting("wifi_pass", CONFIG_CLU_WIFI_PASSWORD);
    if (!s_wifi_ssid[0]) {
        ESP_LOGE(TAG, "no WiFi SSID (cfg partition wifi_ssid or menuconfig CLU_WIFI_SSID): WiFi off");
        return;
    }
    clu_net_init(CLU_NET_WIFI, esp_netif_create_default_wifi_sta());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, s_wifi_ssid, sizeof wifi_config.sta.ssid);
    strlcpy((char *)wifi_config.sta.password, pass, sizeof wifi_config.sta.password);
    // Never join open/WEP/WPA networks (a spoofed AP with the same SSID could otherwise
    // downgrade us); WPA2-PSK minimum, or WPA3-SAE only.
#if CONFIG_CLU_WIFI_WPA3_ONLY
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;
#else
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
#endif
    // SAE: accept both hunt-and-peck and hash-to-element (H2E-only APs refuse HnP).
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_config.sta.pmf_cfg.capable = true;
    // Optional fixed AP (e.g. pin one AP of a mesh / avoid band steering).
    if (CONFIG_CLU_WIFI_BSSID[0]) {
        unsigned bssid_octets[6];
        if (sscanf(CONFIG_CLU_WIFI_BSSID, "%x:%x:%x:%x:%x:%x", &bssid_octets[0], &bssid_octets[1], &bssid_octets[2], &bssid_octets[3], &bssid_octets[4], &bssid_octets[5]) == 6) {
            for (int i = 0; i < 6; i++) wifi_config.sta.bssid[i] = (uint8_t)bssid_octets[i];
            wifi_config.sta.bssid_set = true;
            ESP_LOGW(TAG, "WiFi BSSID locked to " MACSTR, MAC2STR(wifi_config.sta.bssid));
        } else {
            ESP_LOGE(TAG, "CLU_WIFI_BSSID \"%s\" is not aa:bb:cc:dd:ee:ff, ignored", CONFIG_CLU_WIFI_BSSID);
        }
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
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
#endif // CONFIG_CLU_WIFI

/**
 * @brief Bring up the enabled interfaces (WiFi STA and / or Ethernet), each with its own IP config.
 */
static void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL));
#if CONFIG_CLU_ETH
    clu_eth_start();
#endif
#if CONFIG_CLU_WIFI
    wifi_start();
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
    static wl_handle_t wear_level_handle;
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 6,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(CLU_FS_ROOT, "storage", &mount_config, &wear_level_handle);
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
 * Values are also kept in the Lua object. CLU_ZWAVE_2: 18 UseCloud, 22 NTPServer, 23 TimeZone,
 * 25 PrimaryDNS, 26 SecondaryDNS. GATE: 2 PrimaryDNS, 3 SecondaryDNS, 14 TimeZone, 18 UseCloud,
 * 21 UseNTP.
 */
static void on_clu_feature(int idx, const char *str, double num)
{
#if CONFIG_CLU_DEVICE_GATE_HTTP
    switch (idx) {
    case 14: clu_time_set_zone(num >= 0 && num < 1000 ? (int)num : -1); break; // TimeZone
    case 2: clu_net_set_dns(0, str); break;                                 // PrimaryDNS
    case 3: clu_net_set_dns(1, str); break;                                 // SecondaryDNS
    case 18: clu_mqtt_set_enabled(num != 0); break;                         // UseCloud
    case 21: clu_time_set_ntp_enabled(num != 0); break;                     // UseNTP
    default: break;
    }
#else
    switch (idx) {
    case 22: clu_time_set_ntp_server(str); break;       // NTPServer
    case 23: // TimeZone (enum index); NaN / out-of-range doubles must not reach the int cast
        clu_time_set_zone(num >= 0 && num < 1000 ? (int)num : -1);
        break;
    case 25: clu_net_set_dns(0, str); break;            // PrimaryDNS (both interfaces)
    case 26: clu_net_set_dns(1, str); break;            // SecondaryDNS
    case 18: clu_mqtt_set_enabled(num != 0); break;     // UseCloud: MQTT connection on/off
    default: break;
    }
#endif
}

/**
 * @brief GATE ResetReason (feature 26) of this chip boot: 0 power-on, 1 external (reset pin),
 *        2 software restart, 3 exception / watchdog / brownout.
 */
static int boot_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_EXT: return 1;
    case ESP_RST_SW: return 2;
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT: return 3;
    default: return 0;
    }
}

/**
 * @brief clu_lua_cfg_t::on_reload: drop the bus drivers' state and Lua names (modules and their
 * addresses stay; polling goes on).
 *
 * Allows for a full discovery (RESET settle, ADDR_ASSIGN retries, first round) still in progress.
 */
static bool on_lua_reload(void)
{
#if CONFIG_CLU_TFBUS_MASTER
    return tfbus_master_reset(8000);
#else
    return true;
#endif
}

/**
 * @brief clu_lua_cfg_t::on_init (SYSTEM.Init): OM.LUA has configured everything -> driver init, start MQTT.
 */
static void on_lua_init(void)
{
#if CONFIG_CLU_TFBUS_MASTER
    tfbus_master_start();
#endif
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
    const clu_telnet_cfg_t telnet_config = {
        .port = CONFIG_CLU_TELNET_PORT,
        .max_clients = CONFIG_CLU_TELNET_MAX_CLIENTS,
        .history = CONFIG_CLU_TELNET_HISTORY,
        .allow = CONFIG_CLU_TELNET_ALLOW,
    };
    clu_telnet_init(&telnet_config);
#endif
    clu_time_init();
    storage_mount();
    clu_state_write_config_files();

#if CONFIG_CLU_MYSENSORS
    const clu_mysensors_cfg_t msc = {
        .uart_num = CONFIG_CLU_RS485_UART,
        .tx_pin = CONFIG_CLU_RS485_TX_GPIO,
        .rx_pin = CONFIG_CLU_RS485_RX_GPIO,
        .de_pin = CONFIG_CLU_RS485_DE_GPIO,
        .baud = CONFIG_CLU_MYSENSORS_BAUD,
#if CONFIG_CLU_MYSENSORS_METRIC
        .metric = true,
#endif
#if CONFIG_CLU_MYSENSORS_TRACE
        .trace = true,
#endif
        .max_modules = CONFIG_CLU_MYSENSORS_MODULES_MAX,
    };
#endif
#if CONFIG_CLU_TFBUS_MASTER
    const tfbus_uart_cfg_t uart_config = {
#if CONFIG_CLU_TFBUS
        .uart_num = CONFIG_CLU_RS485_UART,
        .tx_pin = CONFIG_CLU_RS485_TX_GPIO,
        .rx_pin = CONFIG_CLU_RS485_RX_GPIO,
        .de_pin = CONFIG_CLU_RS485_DE_GPIO,
        .baud = CONFIG_CLU_TFBUS_BAUD,
#else
        .uart_num = -1, // the RS485 pins carry the MySensors gateway: emulated modules only
#endif
    };
    tfbus_uart_init(&uart_config);
#if CONFIG_CLU_LOCAL_IO
    clu_localio_start(); // answers the bus frames for the on-board IO modules
#endif
#if CONFIG_CLU_MYSENSORS
    clu_mysensors_init(&msc); // the CLU is the RS485 network's gateway (no scripts needed), emulated modules
    tfbus_master_set_log_tag("mysensors"); // the master serves only the emulated modules
    tfbus_master_on_refresh(clu_mysensors_refresh); // OM discovery: modules of nodes that are gone removed
#endif
    tfbus_drivers_register_all();
    tfbus_master_on_scan(clu_state_write_config_files); // CONFIG.JSON tfbusDevices
    tfbus_master_init(); // bus scan starts now, independent of MAIN.LUA / OM.LUA
#endif

    net_start(); // does not wait for an IP: see the file comment
    clu_time_start_sntp();
#if CONFIG_CLU_TELNET
    clu_telnet_start();
#endif
#if CONFIG_CLU_OTA
    const clu_ota_cfg_t ota_config = {
        .port = CONFIG_CLU_OTA_PORT,
        // cfg partition (prebuilt firmware) over menuconfig; empty = no network updates
        .password = clu_state_setting("ota_pass", CONFIG_CLU_OTA_PASSWORD),
        .cors_origin = CONFIG_CLU_OTA_CORS_ORIGIN,
    };
    clu_ota_start(&ota_config);
#endif
    const clu_mqtt_cfg_t mqtt_config = {
        .sn = g_clu.sn,
        .fw_version = CLU_FW_VERSION,
        .ca_path = CLU_FS_ROOT "/a/MQTT_CA.PEM",
        // cfg partition (prebuilt firmware) over menuconfig; NULL = the menuconfig value
        .broker_uri = clu_state_setting("mqtt_uri", NULL),
        .username = clu_state_setting("mqtt_user", NULL),
        .password = clu_state_setting("mqtt_pass", NULL),
    };
    clu_mqtt_init(&mqtt_config);
#if CONFIG_CLU_HTTP
    const clu_http_cfg_t http_config = {
        .port = CONFIG_CLU_HTTP_PORT,
        .max_body = CONFIG_CLU_HTTP_MAX_BODY,
        .listener_timeout_ms = CONFIG_CLU_HTTP_LISTENER_TIMEOUT_MS,
    };
    clu_http_init(&http_config); // HttpRequest / HttpListener (http.lua)
    clu_http_start();
#endif
    clu_sysinfo_init();
    clu_tftp_init(CLU_FS_ROOT);
    udp_server_start(); // before Lua: OM.LUA's remote calls / reports need its queue and lock
    const clu_lua_cfg_t lua_config = {
        .root = CLU_FS_ROOT "/a",
        .sn = g_clu.sn,
        .fw_version = CLU_FW_VERSION,
        .gate = CLU_IS_GATE,
        .reset_reason = boot_reset_reason(),
        .report = udp_server_send_report,
        .on_init = on_lua_init,
        .remote = udp_server_remote_exec,
        .clu_feature = on_clu_feature,
        .cloud_connected = clu_mqtt_connected,
        .on_reload = on_lua_reload,
        .on_time_set = clu_time_store,
    };
    clu_lua_start(&lua_config);
}
