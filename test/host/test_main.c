// Host tests for the boot wiring (main/main.c, included: handlers are static). Every ESP-IDF and
// module call is a stub that appends to a call log, so the tests check the boot order (e.g. the
// PIN is logged before the telnet log hook exists), the configuration handed to each module, the
// WiFi event handling and the CLU feature -> module mapping.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *cfg_bssid = "";
#define CONFIG_CLU_WIFI_SSID          "HomeNet"
#define CONFIG_CLU_WIFI_PASSWORD      "secretpw"
#define CONFIG_CLU_WIFI_BSSID         cfg_bssid
#define CONFIG_CLU_WIFI_MAX_TX_POWER  60
#define CONFIG_CLU_TELNET             1
#define CONFIG_CLU_TELNET_PORT        23
#define CONFIG_CLU_TELNET_MAX_CLIENTS 2
#define CONFIG_CLU_TELNET_HISTORY     4096
#define CONFIG_CLU_TELNET_ALLOW       "192.168.1.0/24"
#define CONFIG_CLU_RS485_UART         1
#define CONFIG_CLU_RS485_TX_GPIO      17
#define CONFIG_CLU_RS485_RX_GPIO      18
#define CONFIG_CLU_RS485_DE_GPIO      8
#define CONFIG_CLU_RS485_BAUD         115200

// storage_mount() creates the drive directories under /fs: never on the host
static char s_mkdirs[128];
static int test_mkdir(const char *p, mode_t m)
{
    strcat(s_mkdirs, p);
    strcat(s_mkdirs, " ");
    return 0;
}
#define mkdir test_mkdir

#include "../../main/main.c"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

static char s_log[4096];
static void L(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(s_log);
    vsnprintf(s_log + n, sizeof s_log - n, fmt, ap);
    va_end(ap);
    strncat(s_log, " ", sizeof s_log - strlen(s_log) - 1);
}

static void check_log(const char *name, const char *want)
{
    int ok = strcmp(s_log, want) == 0;
    check(name, ok);
    if (!ok) {
        printf("  got  %s\n  want %s\n", s_log, want);
    }
    s_log[0] = 0;
}

// ---------------------------------------------------------------- stubs
clu_state_t g_clu = {.sn = 330017203};
esp_event_base_t const WIFI_EVENT = "WIFI_EVENT", IP_EVENT = "IP_EVENT";

static esp_err_t s_nvs_init[2] = {ESP_OK, ESP_OK};
static int s_nvs_inits;
esp_err_t nvs_flash_init(void) { L("nvs_init"); return s_nvs_init[s_nvs_inits++ % 2]; }
esp_err_t nvs_flash_erase(void) { L("nvs_erase"); return ESP_OK; }
esp_err_t clu_crypto_init(void) { L("crypto"); return ESP_OK; }
void clu_state_load(void) { L("state_load"); }
// NVS cfg/* settings: key -> value (NULL = not provisioned)
static const char *s_set_wifi_ssid, *s_set_wifi_pass, *s_set_mqtt_uri = "mqtt://10.0.0.5", *s_set_mqtt_user;
const char *clu_state_setting(const char *key, const char *fallback)
{
    const char *v = !strcmp(key, "wifi_ssid") ? s_set_wifi_ssid
                  : !strcmp(key, "wifi_pass") ? s_set_wifi_pass
                  : !strcmp(key, "mqtt_uri")  ? s_set_mqtt_uri
                  : !strcmp(key, "mqtt_user") ? s_set_mqtt_user : NULL;
    return v ? v : fallback;
}
static clu_telnet_cfg_t s_tc;
void clu_telnet_init(const clu_telnet_cfg_t *c) { s_tc = *c; L("telnet_init"); }
void clu_telnet_start(void) { L("telnet_start"); }
void clu_time_init(void) { L("time_init"); }
void clu_time_store(void) {}
void clu_time_start_sntp(void) { L("sntp"); }
void clu_time_set_ntp_server(const char *s) { L("ntp(%s)", s); }
void clu_time_set_zone(int z) { L("zone(%d)", z); }
static esp_err_t s_mount = ESP_OK;
esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *base, const char *label, const esp_vfs_fat_mount_config_t *c,
                                           wl_handle_t *wl)
{
    L("mount(%s,%s,%d)", base, label, c->format_if_mount_failed);
    return s_mount;
}
void clu_state_write_config_files(void) { L("config_files"); }
static tfbus_uart_cfg_t s_uc;
esp_err_t tfbus_uart_init(const tfbus_uart_cfg_t *c) { s_uc = *c; L("uart"); return ESP_OK; }
void tfbus_drivers_register_all(void) { L("drivers"); }
void tfbus_master_init(void) { L("master_init"); }
static void (*s_scan_cb)(void);
void tfbus_master_on_scan(void (*cb)(void)) { s_scan_cb = cb; L("on_scan"); }
void tfbus_master_start(void) { L("master_start"); }
static bool s_reset_ok = true;
bool tfbus_master_reset(uint32_t ms) { L("master_reset(%u)", (unsigned)ms); return s_reset_ok; }

bool sntp_restart(void) { L("sntp_restart"); return true; }

static esp_netif_t *const STA = (esp_netif_t *)0x5a;
esp_err_t esp_netif_init(void) { L("netif"); return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return STA; }
void clu_net_init(clu_net_if_t i, esp_netif_t *n)
{
    L("net_init(%s,%s)", i == CLU_NET_WIFI ? "wifi" : "eth", n == STA ? "sta" : "?");
}
void clu_net_set_dns(int i, const char *a) { L("dns%d(%s)", i, a); }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
static esp_event_handler_t s_wifi_handler, s_ip_handler;
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t fn, void *arg)
{
    *(base == IP_EVENT ? &s_ip_handler : &s_wifi_handler) = fn;
    L("handler(%s,%d)", base, (int)id);
    return ESP_OK;
}

static wifi_config_t s_wc;
static esp_err_t s_scan_rc = ESP_OK;
esp_err_t esp_wifi_init(const wifi_init_config_t *c) { L("wifi_init"); return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t m) { return ESP_OK; }
esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c) { s_wc = *c; L("wifi_config"); return ESP_OK; }
esp_err_t esp_wifi_get_config(wifi_interface_t i, wifi_config_t *c) { *c = s_wc; return ESP_OK; }
esp_err_t esp_wifi_set_ps(wifi_ps_type_t t) { L("ps(%d)", t); return ESP_OK; }
esp_err_t esp_wifi_start(void) { L("wifi_start"); return ESP_OK; }
esp_err_t esp_wifi_set_max_tx_power(int8_t p) { L("txpower(%d)", p); return ESP_OK; }
esp_err_t esp_wifi_get_max_tx_power(int8_t *p) { *p = 60; return ESP_OK; }
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *c, bool block)
{
    L("scan(%s,%d)", (const char *)c->ssid, c->show_hidden);
    return s_scan_rc;
}
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *ap)
{
    *n = 2;
    memset(ap, 0, 2 * sizeof *ap);
    ap[0].primary = 6;
    ap[1].primary = 11;
    L("ap_records");
    return ESP_OK;
}
esp_err_t esp_wifi_connect(void) { L("connect"); return ESP_OK; }
esp_err_t esp_wifi_sta_get_rssi(int *r) { return ESP_FAIL; }

static clu_mqtt_cfg_t s_mc;
void clu_mqtt_init(const clu_mqtt_cfg_t *c) { s_mc = *c; L("mqtt_init"); }
void clu_mqtt_start(void) { L("mqtt_start"); }
void clu_mqtt_set_enabled(bool on) { L("mqtt(%d)", on); }
bool clu_mqtt_connected(void) { return false; }
void clu_sysinfo_init(void) { L("sysinfo"); }
static clu_lua_cfg_t s_lc;
void clu_lua_start(const clu_lua_cfg_t *c) { s_lc = *c; L("lua_start"); }
void clu_tftp_init(const char *root) { L("tftp(%s)", root); }
void udp_server_start(void) { L("udp_start"); }
void udp_server_net_up(void) { L("net_up"); }
void udp_server_send_report(const char *a, int p, const char *b) {}
uint32_t udp_server_remote_exec(uint32_t ip, const char *s) { return 0; }

// ---------------------------------------------------------------- tests
#if CONFIG_CLU_TFBUS
#define BUS_BOOT "uart drivers on_scan master_init "
#else
#define BUS_BOOT ""
#endif

int main(void)
{
    app_main();
    check_log("boot order: NVS, crypto, identity (PIN logged) before the telnet hook, then storage, bus, WiFi "
              "(no wait for an IP), services, TFTP, UDP before Lua",
              "nvs_init crypto state_load telnet_init time_init mount(/fs,storage,1) config_files " BUS_BOOT
              "netif handler(IP_EVENT,0) handler(IP_EVENT,4) net_init(wifi,sta) wifi_init handler(WIFI_EVENT,-1) "
              "wifi_config ps(0) wifi_start txpower(60) sntp telnet_start mqtt_init sysinfo tftp(/fs) udp_start "
              "lua_start ");
    check("storage: a:\\ and p:\\ created", !strcmp(s_mkdirs, "/fs/a /fs/p "));
    check("telnet config from Kconfig", s_tc.port == 23 && s_tc.max_clients == 2 && s_tc.history == 4096 &&
                                            !strcmp(s_tc.allow, "192.168.1.0/24"));
#if CONFIG_CLU_TFBUS
    check("RS485 config from Kconfig", s_uc.uart_num == 1 && s_uc.tx_pin == 17 && s_uc.rx_pin == 18 &&
                                           s_uc.de_pin == 8 && s_uc.baud == 115200);
    check("bus scans rewrite CONFIG.JSON / config.txt", s_scan_cb == clu_state_write_config_files);
#else
    check("CLU_TFBUS off: no UART, drivers or bus master", !s_scan_cb && !strstr(s_log, "uart"));
#endif
    check("WiFi: SSID / password, WPA2-PSK minimum, SAE both, PMF capable, no BSSID lock",
          !strcmp((char *)s_wc.sta.ssid, "HomeNet") && !strcmp((char *)s_wc.sta.password, "secretpw") &&
              s_wc.sta.threshold.authmode == WIFI_AUTH_WPA2_PSK && s_wc.sta.sae_pwe_h2e == WPA3_SAE_PWE_BOTH &&
              s_wc.sta.pmf_cfg.capable && !s_wc.sta.bssid_set);
    check("MQTT: serial, firmware version, CA path on a:\\",
          s_mc.sn == 330017203 && !strcmp(s_mc.fw_version, "1.0.0") && !strcmp(s_mc.ca_path, "/fs/a/MQTT_CA.PEM"));
    check("MQTT: broker URI from NVS, unset user / password left to menuconfig (NULL)",
          s_mc.broker_uri && !strcmp(s_mc.broker_uri, "mqtt://10.0.0.5") && !s_mc.username && !s_mc.password);
    check("Lua: root a:\\, serial, transport / feature / reload hooks wired",
          !strcmp(s_lc.root, "/fs/a") && s_lc.sn == 330017203 && s_lc.report == udp_server_send_report &&
              s_lc.remote == udp_server_remote_exec && s_lc.cloud_connected == clu_mqtt_connected &&
              s_lc.on_init == on_lua_init && s_lc.on_reload == on_lua_reload && s_lc.clu_feature == on_clu_feature &&
              s_lc.on_time_set == clu_time_store);

    // NVS partition full / from a newer IDF: erased and initialised again; mount failure tolerated
    s_nvs_init[0] = ESP_ERR_NVS_NO_FREE_PAGES;
    s_nvs_inits = 0;
    s_mount = ESP_FAIL;
    s_mkdirs[0] = 0;
    app_main();
    check("NVS without free pages: erased, initialised again", !strncmp(s_log, "nvs_init nvs_erase nvs_init crypto", 34));
    check("storage mount failure: no drive directories, boot continues", !s_mkdirs[0] && strstr(s_log, "lua_start"));
    s_log[0] = 0;
    s_mount = ESP_OK;

    // WiFi events
    s_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    s_scan_rc = ESP_FAIL;
    s_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    s_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_SCAN_DONE, NULL);
    check_log("STA start: diagnostic scan (connect at once if it cannot start); scan done: log APs, connect",
              "scan(HomeNet,1) scan(HomeNet,1) connect ap_records connect ");
    wifi_event_sta_connected_t con = {.ssid = "HomeNet", .ssid_len = 7, .channel = 6, .authmode = 3};
    s_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &con);
    ip_event_got_ip_t ip = {.ip_info.ip.addr = 0x1401a8c0};
    s_log[0] = 0;
    s_ip_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    check_log("WiFi got IP: setup window armed, SNTP polls now", "net_up sntp_restart mqtt_start ");
    s_ip_handler(NULL, IP_EVENT, IP_EVENT_ETH_GOT_IP, &ip);
    check_log("Ethernet got IP: the same", "net_up sntp_restart mqtt_start ");

#if CONFIG_CLU_WIFI
    // ---- prebuilt firmware: WiFi credentials from NVS win over menuconfig; no SSID -> WiFi off
    s_set_wifi_ssid = "NvsNet";
    s_set_wifi_pass = "nvspass1";
    memset(&s_wc, 0, sizeof s_wc);
    wifi_start();
    check("WiFi: SSID / password from NVS",
          !strcmp((char *)s_wc.sta.ssid, "NvsNet") && !strcmp((char *)s_wc.sta.password, "nvspass1"));
    s_log[0] = 0;
    s_scan_rc = ESP_OK;
    s_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    check_log("WiFi: diagnostic scan for the NVS SSID", "scan(NvsNet,1) ");
    s_set_wifi_ssid = "";
    s_log[0] = 0;
    wifi_start();
    check_log("WiFi: no SSID anywhere -> WiFi stays off", "");
    s_set_wifi_ssid = s_set_wifi_pass = NULL;
#endif
    wifi_event_sta_disconnected_t dis = {.reason = 201, .rssi = -80};
    s_log[0] = 0;
    s_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &dis);
    check_log("disconnected: reconnect", "connect ");
    s_log[0] = 0;
    s_wifi_handler(NULL, WIFI_EVENT, 99, NULL);
    check_log("other events ignored", "");

    // BSSID lock
    cfg_bssid = "AA:bb:0c:1d:2e:3f";
    wifi_start();
    check("BSSID configured: AP locked", s_wc.sta.bssid_set && s_wc.sta.bssid[0] == 0xaa && s_wc.sta.bssid[5] == 0x3f);
    cfg_bssid = "aa:bb:cc";
    wifi_start();
    check("malformed BSSID: ignored", !s_wc.sta.bssid_set);
    s_log[0] = 0;

    // CLU features -> modules
    on_clu_feature(22, "ntp.example", 0);
    on_clu_feature(23, NULL, 5);
    on_clu_feature(23, NULL, NAN);
    on_clu_feature(23, NULL, 1e9);
    on_clu_feature(23, NULL, -3);
    on_clu_feature(25, "8.8.8.8", 0);
    on_clu_feature(26, "1.1.1.1", 0);
    on_clu_feature(18, NULL, 0);
    on_clu_feature(18, NULL, 1);
    on_clu_feature(99, "x", 1);
    check_log("features: NTPServer, TimeZone (NaN / out of range -> -1), DNS 1/2, UseCloud; others ignored",
              "ntp(ntp.example) zone(5) zone(-1) zone(-1) zone(-1) dns0(8.8.8.8) dns1(1.1.1.1) mqtt(0) mqtt(1) ");

    // Lua lifecycle hooks
    on_lua_init();
#if CONFIG_CLU_TFBUS
    check("SYSTEM.Init: bus polling and MQTT start", !strcmp(s_log, "master_start mqtt_start ") && (s_log[0] = 0, 1));
    check("reload: bus registry reset (8 s)", on_lua_reload() && !strcmp(s_log, "master_reset(8000) "));
    s_reset_ok = false;
    check("reload: bus reset failure reported", !on_lua_reload());
#else
    check("CLU_TFBUS off: SYSTEM.Init starts only MQTT", !strcmp(s_log, "mqtt_start ") && (s_log[0] = 0, 1));
    s_reset_ok = false;
    check("CLU_TFBUS off: reload succeeds, bus untouched", on_lua_reload() && !s_log[0]);
#endif

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
