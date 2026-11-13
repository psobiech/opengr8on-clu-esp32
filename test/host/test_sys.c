// Host tests for the system helpers in main/: clu_net (static IP / DNS), clu_time (time zone,
// SNTP), clu_state (identity, keys, CONFIG.JSON / config.txt), clu_diag (diagnostic pack) and
// clu_sysinfo (Lua sys_info / tfbus_module / sys_restart). The sources are built with
// sys_host.h force-included: "/fs/..." maps to a temporary directory, CONFIG_CLU_* to variables.
#define _GNU_SOURCE // nftw
#include <arpa/inet.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clu_diag.h"
#include "clu_keys.h"
#include "clu_lua.h"
#include "clu_mqtt.h"
#include "clu_net.h"
#include "clu_rtc.h"
#include "clu_state.h"
#include "clu_sysinfo.h"
#include "clu_time.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "fake_nvs.h"
#include "lauxlib.h"
#include "lualib.h"
#include "tfbus_master.h"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

static void check_str(const char *name, const char *got, const char *want)
{
    int ok = got && strcmp(got, want) == 0;
    check(name, ok);
    if (!ok) {
        printf("  got  %s\n  want %s\n", got ? got : "(null)", want);
    }
}

// ---------------------------------------------------------------- /fs -> temp dir, Kconfig
static char s_root[256];
const char *host_cfg_serial = "", *host_cfg_pin = "", *host_cfg_iv = "";

static const char *fs(const char *path, char *buf, size_t cap)
{
    if (strncmp(path, "/fs/", 4) == 0) {
        snprintf(buf, cap, "%s/%s", s_root, path + 4);
        return buf;
    }
    return path;
}
FILE *host_fopen(const char *path, const char *mode)
{
    char b[512];
    return fopen(fs(path, b, sizeof b), mode);
}
int host_mkdir(const char *path, mode_t mode)
{
    char b[512];
    return mkdir(fs(path, b, sizeof b), mode);
}
int host_stat(const char *path, struct stat *st)
{
    char b[512];
    return stat(fs(path, b, sizeof b), st);
}

static char *slurp(const char *rel)
{
    static char buf[8192];
    char p[512];
    snprintf(p, sizeof p, "%s/%s", s_root, rel);
    FILE *f = fopen(p, "r");
    if (!f) {
        return NULL;
    }
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static void put_file(const char *rel, const char *content)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", s_root, rel);
    FILE *f = fopen(p, "w");
    fputs(content, f);
    fclose(f);
}

// ---------------------------------------------------------------- platform stubs
static uint8_t s_rnd = 0x10;
static int s_rnd_zero; // next N clu_random calls return zeros
void clu_random(uint8_t *out, size_t len)
{
    if (s_rnd_zero > 0) {
        s_rnd_zero--;
        memset(out, 0, len);
        return;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = s_rnd++;
    }
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    static const uint8_t m[6] = {0x24, 0x6f, 0x28, 0xab, 0xcd, 0xef};
    memcpy(mac, m, 6);
    return ESP_OK;
}

// netif
static esp_netif_t *const NETIF = (esp_netif_t *)0x1234;
static esp_netif_t *const ETH_NETIF = (esp_netif_t *)0x5678;
static int s_dhcp_start, s_dhcp_stop;
static esp_netif_ip_info_t s_ipinfo, s_ipinfo_eth;
static uint32_t s_dns_main, s_dns_backup;
static bool s_netif_present = true;
esp_err_t esp_netif_dhcpc_start(esp_netif_t *n) { s_dhcp_start++; return ESP_OK; }
esp_err_t esp_netif_dhcpc_stop(esp_netif_t *n) { s_dhcp_stop++; return ESP_OK; }
esp_err_t esp_netif_set_ip_info(esp_netif_t *n, const esp_netif_ip_info_t *ii)
{
    *(n == ETH_NETIF ? &s_ipinfo_eth : &s_ipinfo) = *ii;
    return ESP_OK;
}
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ii)
{
    *ii = n == ETH_NETIF ? s_ipinfo_eth : s_ipinfo;
    return ESP_OK;
}
esp_err_t esp_netif_set_dns_info(esp_netif_t *n, esp_netif_dns_type_t t, esp_netif_dns_info_t *d)
{
    *(t == ESP_NETIF_DNS_MAIN ? &s_dns_main : &s_dns_backup) = d->ip.u_addr.ip4.addr;
    return ESP_OK;
}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{
    return s_netif_present && !strcmp(key, "WIFI_STA_DEF") ? NETIF : NULL;
}
char *esp_ip4addr_ntoa(const esp_ip4_addr_t *a, char *buf, int len)
{
    struct in_addr x = {.s_addr = a->addr};
    return (char *)inet_ntop(AF_INET, &x, buf, (socklen_t)len);
}

// wall clock (sys_host.h) and RTC
static time_t s_now = 1000; // system clock: not set (1970) unless a test says otherwise
static time_t s_settime = -1;
int host_settimeofday(const struct timeval *tv, const void *tz) { s_now = s_settime = tv->tv_sec; return 0; }
time_t host_time(time_t *t) { if (t) *t = s_now; return s_now; }
static bool s_rtc_present = true, s_rtc_valid;
static int s_rtc_sda = -2, s_rtc_scl = -2, s_rtc_sets;
static clu_rtc_chip_t s_rtc_chip = (clu_rtc_chip_t)-1;
static time_t s_rtc_time;
bool clu_rtc_init(clu_rtc_chip_t chip, int sda, int scl) { s_rtc_chip = chip; s_rtc_sda = sda; s_rtc_scl = scl; return s_rtc_present; }
bool clu_rtc_get(time_t *t) { if (!s_rtc_valid) return false; *t = s_rtc_time; return true; }
bool clu_rtc_set(time_t t) { s_rtc_sets++; s_rtc_time = t; s_rtc_valid = true; return true; }

// sntp
static char s_sntp_server[64];
static int s_sntp_init, s_sntp_stop;
static void (*s_sntp_cb)(struct timeval *);
void esp_sntp_stop(void) { s_sntp_stop++; }
void esp_sntp_setoperatingmode(esp_sntp_operatingmode_t m) {}
void esp_sntp_setservername(int idx, const char *server) { snprintf(s_sntp_server, sizeof s_sntp_server, "%s", server); }
void sntp_set_time_sync_notification_cb(void (*cb)(struct timeval *tv)) { s_sntp_cb = cb; }
void esp_sntp_init(void) { s_sntp_init++; }

// system
static esp_reset_reason_t s_reason = ESP_RST_POWERON;
static int s_restarts, s_rssi_ok = 1;
esp_reset_reason_t esp_reset_reason(void) { return s_reason; }
uint32_t esp_get_free_heap_size(void) { return 123456; }
uint32_t esp_get_minimum_free_heap_size(void) { return 100000; }
void esp_restart(void) { s_restarts++; }
esp_err_t esp_wifi_sta_get_rssi(int *rssi)
{
    *rssi = -61;
    return s_rssi_ok ? ESP_OK : ESP_FAIL;
}

// esp_timer: one-shot timers fired by the test
static esp_timer_create_args_t s_timer_args;
static int s_timer_fail, s_timer_starts;
static int64_t s_timer_us;
int64_t esp_timer_get_time(void) { return 0; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h)
{
    if (s_timer_fail) {
        return ESP_FAIL;
    }
    s_timer_args = *a;
    *h = (esp_timer_handle_t)&s_timer_args;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us)
{
    s_timer_starts++;
    s_timer_us = (int64_t)us;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t h) { return ESP_OK; }
esp_err_t esp_timer_delete(esp_timer_handle_t h) { return ESP_OK; }

// clu_mqtt / clu_lua
static bool s_mqtt_up;
bool clu_mqtt_connected(void) { return s_mqtt_up; }
static const luaL_Reg *s_ext;
static char s_ext_name[32];
void clu_lua_register_ext(const luaL_Reg *fns, const char *src, size_t len, const char *name)
{
    s_ext = fns;
    snprintf(s_ext_name, sizeof s_ext_name, "%s", name);
}
static int s_shutdown_hooks;
void clu_lua_shutdown_hooks(lua_State *L) { s_shutdown_hooks++; }

// tfbus_master: registry + last scan
static tfbus_module_t s_mods[3];
static int s_nmods;
static tfbus_device_t s_devs[3];
static int s_ndevs;
int tfbus_master_count(void) { return s_nmods; }
tfbus_module_t *tfbus_master_get(int i) { return i >= 0 && i < s_nmods ? &s_mods[i] : NULL; }
tfbus_module_t *tfbus_master_find_sn(uint32_t sn)
{
    for (int i = 0; i < s_nmods; i++) {
        if (s_mods[i].sn == sn) {
            return &s_mods[i];
        }
    }
    return NULL;
}
int tfbus_master_devices(tfbus_device_t *out, int max)
{
    int n = s_ndevs < max ? s_ndevs : max;
    memcpy(out, s_devs, (size_t)n * sizeof *out);
    return n;
}

// ---------------------------------------------------------------- clu_net
static uint32_t nvs_u32(const char *ns, const char *k)
{
    const nvs_entry_t *e = nvs_peek(ns, k);
    uint32_t v = 0;
    if (e) {
        memcpy(&v, e->val, 4);
    }
    return v;
}

static void test_net(void)
{
    nvs_reset();
    clu_net_apply(CLU_NET_WIFI);
    check("net: apply before init does nothing", s_dhcp_start == 0 && s_dhcp_stop == 0);
    nvs_fail_open = 1;
    clu_net_init(CLU_NET_WIFI, NETIF);
    nvs_fail_open = 0;
    check("net: init without stored config -> DHCP", s_dhcp_start == 1 && s_dhcp_stop == 0);

    check("net: invalid IP rejected", !clu_net_set_static(CLU_NET_WIFI, "not-an-ip", "192.168.1.1"));
    check("net: invalid gateway rejected", !clu_net_set_static(CLU_NET_WIFI, "192.168.1.77", "gw?"));
    check("net: static IP accepted", clu_net_set_static(CLU_NET_WIFI, "192.168.1.77", "192.168.1.1"));
    check("net: static IP, gateway and /24 mask stored",
          nvs_u32("clu_net", "ip") == inet_addr("192.168.1.77") && nvs_u32("clu_net", "gw") == inet_addr("192.168.1.1") &&
              nvs_u32("clu_net", "mask") == inet_addr("255.255.255.0") && nvs_commits == 1);
    clu_net_apply(CLU_NET_WIFI);
    check("net: static apply stops DHCP, sets address / gateway / mask, gateway as DNS",
          s_dhcp_stop == 1 && s_ipinfo.ip.addr == inet_addr("192.168.1.77") &&
              s_ipinfo.gw.addr == inet_addr("192.168.1.1") && s_ipinfo.netmask.addr == inet_addr("255.255.255.0") &&
              s_dns_main == inet_addr("192.168.1.1") && s_dns_backup == 0);

    clu_net_set_static(CLU_NET_WIFI, "10.0.0.5", "10.0.1.1");
    check("net: gateway outside the /24 -> no netmask (all on-link)", nvs_u32("clu_net", "mask") == 0);
    clu_net_set_static(CLU_NET_WIFI, "10.0.0.6", "");
    check("net: empty gateway allowed", nvs_u32("clu_net", "ip") == inet_addr("10.0.0.6") && nvs_u32("clu_net", "gw") == 0);

    int commits = nvs_commits;
    clu_net_set_dns(0, "8.8.8.8");
    clu_net_set_dns(0, "8.8.8.8"); // unchanged: not saved again
    clu_net_set_dns(1, "1.1.1.1");
    clu_net_set_dns(2, "9.9.9.9");
    clu_net_set_dns(1, NULL);
    clu_net_set_dns(1, "bad");
    check("net: DNS servers stored (duplicates / bad index / bad address ignored)",
          nvs_commits == commits + 2 && nvs_u32("clu_net", "dns1") == inet_addr("8.8.8.8") &&
              nvs_u32("clu_net", "dns2") == inet_addr("1.1.1.1"));
    check("net: DNS applied at once with a static IP",
          s_dns_main == inet_addr("8.8.8.8") && s_dns_backup == inet_addr("1.1.1.1"));

    check("net: 0.0.0.0 -> back to DHCP", clu_net_set_static(CLU_NET_WIFI, "0.0.0.0", NULL) && nvs_u32("clu_net", "ip") == 0);
    s_dns_main = 0;
    clu_net_set_dns(0, "8.8.4.4");
    check("net: DNS under DHCP stored, not applied", nvs_u32("clu_net", "dns1") == inet_addr("8.8.4.4") && s_dns_main == 0);
    int starts = s_dhcp_start;
    clu_net_apply(CLU_NET_WIFI);
    check("net: apply under DHCP restarts the DHCP client", s_dhcp_start == starts + 1);

    // boot with a stored static config
    clu_net_set_static(CLU_NET_WIFI, "192.168.5.9", "192.168.5.1");
    clu_net_init(CLU_NET_WIFI, NETIF);
    check("net: stored static config applied at boot, stored DNS used",
          s_ipinfo.ip.addr == inet_addr("192.168.5.9") && s_dns_main == inet_addr("8.8.4.4") &&
              s_dns_backup == inet_addr("1.1.1.1"));
    nvs_fail_open = 1;
    check("net: NVS unavailable: change still accepted in RAM", clu_net_set_static(CLU_NET_WIFI, "192.168.5.10", NULL));
    nvs_fail_open = 0;

    // Ethernet: own config in "clu_net_eth", DNS shared
    nvs_reset();
    memset(&s_ipinfo_eth, 0, sizeof s_ipinfo_eth);
    clu_net_set_dns(0, "9.9.9.9");
    check("net eth: static IP accepted", clu_net_set_static(CLU_NET_ETH, "192.168.7.5", "192.168.7.1"));
    check("net eth: stored under its own namespace, WiFi config untouched",
          nvs_u32("clu_net_eth", "ip") == inet_addr("192.168.7.5") && nvs_u32("clu_net_eth", "gw") == inet_addr("192.168.7.1") &&
              !nvs_peek("clu_net", "ip"));
    clu_net_init(CLU_NET_ETH, ETH_NETIF);
    check("net eth: init applies it to the Ethernet netif with the shared DNS",
          s_ipinfo_eth.ip.addr == inet_addr("192.168.7.5") && s_dns_main == inet_addr("9.9.9.9"));

    // interface selection: WiFi 10.1.0.9/24, Ethernet 192.168.7.5/24
    s_ipinfo = (esp_netif_ip_info_t){.ip.addr = inet_addr("10.1.0.9"), .netmask.addr = inet_addr("255.255.255.0")};
    check("net: peer on the WiFi subnet -> WiFi", clu_net_if_for(inet_addr("10.1.0.50")) == CLU_NET_WIFI &&
                                                     clu_net_local_ip(inet_addr("10.1.0.50")) == inet_addr("10.1.0.9"));
    check("net: peer on the Ethernet subnet -> Ethernet", clu_net_if_for(inet_addr("192.168.7.50")) == CLU_NET_ETH);
    check("net: peer elsewhere / no peer -> Ethernet first",
          clu_net_if_for(inet_addr("172.16.0.1")) == CLU_NET_ETH && clu_net_if_for(0) == CLU_NET_ETH);
    s_ipinfo_eth.ip.addr = 0;
    check("net: Ethernet without address -> WiFi", clu_net_if_for(inet_addr("172.16.0.1")) == CLU_NET_WIFI);
    s_ipinfo.ip.addr = 0;
    check("net: no address at all -> Ethernet (started), our IP 0",
          clu_net_if_for(inet_addr("172.16.0.1")) == CLU_NET_ETH && clu_net_local_ip(0) == 0);
    memset(&s_ipinfo, 0, sizeof s_ipinfo);
}

// ---------------------------------------------------------------- clu_time
static void test_time(void)
{
    nvs_reset();
    clu_time_init();
    check_str("time: default zone Europe/Warsaw", getenv("TZ"), "CET-1CEST,M3.5.0,M10.5.0/3");
    clu_time_set_ntp_server("ntp1.example");
    check("time: NTP server stored, SNTP not running yet -> not started",
          nvs_peek("clu_time", "ntp") && !strcmp((const char *)nvs_peek("clu_time", "ntp")->val, "ntp1.example") &&
              s_sntp_init == 0);
    clu_time_start_sntp();
    check("time: SNTP started with the server, sync callback set",
          s_sntp_init == 1 && s_sntp_stop == 0 && !strcmp(s_sntp_server, "ntp1.example") && s_sntp_cb);
    s_sntp_cb(NULL);
    clu_time_set_ntp_server("ntp1.example");
    clu_time_set_ntp_server("");
    clu_time_set_ntp_server(NULL);
    check("time: same / empty server ignored", s_sntp_init == 1);
    clu_time_set_ntp_server("ntp2.example");
    check("time: new server restarts SNTP", s_sntp_init == 2 && s_sntp_stop == 1 && !strcmp(s_sntp_server, "ntp2.example"));

    clu_time_set_zone(0);
    int commits = nvs_commits;
    check("time: same zone ignored", commits == nvs_commits);
    clu_time_set_zone(22);
    check_str("time: zone 22 -> UTC", getenv("TZ"), "UTC0");
    check("time: zone stored", nvs_peek("clu_time", "tz") && nvs_peek("clu_time", "tz")->val[0] == 22);
    clu_time_set_zone(99);
    check_str("time: unknown zone -> Europe/Warsaw", getenv("TZ"), "CET-1CEST,M3.5.0,M10.5.0/3");

    // boot with stored settings, then with an empty stored server, then without NVS
    clu_time_set_zone(23);
    clu_time_init();
    check_str("time: stored zone applied at boot", getenv("TZ"), "IST-5:30");
    clu_time_start_sntp();
    check_str("time: stored NTP server used", s_sntp_server, "ntp2.example");
    nvs_handle_t h;
    nvs_open("clu_time", NVS_READWRITE, &h);
    nvs_set_str(h, "ntp", "");
    clu_time_init();
    clu_time_start_sntp();
    check_str("time: empty stored NTP server -> pool.ntp.org", s_sntp_server, "pool.ntp.org");
    nvs_fail_open = 1;
    clu_time_set_zone(8);
    clu_time_init();
    nvs_fail_open = 0;
    check_str("time: no NVS at boot -> default zone", getenv("TZ"), "CET-1CEST,M3.5.0,M10.5.0/3");
}

static void test_time_rtc(void)
{
    nvs_reset();
    // valid RTC: system clock set from it at boot
    s_now = 1000;
    s_rtc_valid = true;
    s_rtc_time = 1750000000;
    s_settime = -1;
    s_rtc_sets = 0;
    clu_time_init();
    check("rtc: Kconfig chip (PCF85063) and pins used", s_rtc_chip == CLU_RTC_PCF85063 && s_rtc_sda == 39 && s_rtc_scl == 38);
    check("rtc: valid time -> system clock set from it, RTC not rewritten",
          s_settime == 1750000000 && s_rtc_sets == 0);

    // RTC lost power, system clock kept across a software reset: RTC seeded from it
    s_rtc_valid = false;
    s_now = 1760000000;
    s_settime = -1;
    clu_time_init();
    check("rtc: invalid, system clock plausible -> RTC seeded, clock untouched",
          s_settime == -1 && s_rtc_sets == 1 && s_rtc_time == 1760000000);

    // cold boot, no backup cell: nothing to restore, nothing written
    s_rtc_valid = false;
    s_now = 1000;
    s_rtc_sets = 0;
    clu_time_init();
    check("rtc: invalid, clock not set -> both left alone", s_settime == -1 && s_rtc_sets == 0 && s_now == 1000);

    // no chip: nothing read or written
    s_rtc_present = false;
    s_rtc_valid = true;
    s_rtc_time = 1750000000;
    clu_time_init();
    check("rtc: no chip -> clock untouched", s_settime == -1 && s_now == 1000);
    s_rtc_present = true;

    // SNTP sync and SetDateTime write the RTC back
    clu_time_start_sntp();
    struct timeval tv = {.tv_sec = 1770000000};
    s_rtc_sets = 0;
    s_sntp_cb(&tv);
    check("rtc: SNTP sync written to the RTC", s_rtc_sets == 1 && s_rtc_time == 1770000000);
    s_now = 1780000000;
    clu_time_store();
    check("rtc: clu_time_store writes the system clock", s_rtc_sets == 2 && s_rtc_time == 1780000000);
    s_now = 1000;
    s_rtc_valid = false;
}

// ---------------------------------------------------------------- clu_state
static const char *nvs_str(const char *ns, const char *k)
{
    const nvs_entry_t *e = nvs_peek(ns, k);
    return e && e->type == 's' ? (const char *)e->val : NULL;
}

/** @brief Prebuilt firmware with a settings image in the "cfg" partition (tools/mknvs.py). */
static void cfg_put(const char *pin, uint32_t sn)
{
    nvs_handle_t h;
    nvs_open_from_partition("cfg", "cfg", NVS_READWRITE, &h);
    if (sn) {
        nvs_set_u32(h, "sn", sn);
    }
    if (pin) {
        nvs_set_str(h, "pin", pin);
    }
}

static void test_provisioned(void)
{
    const char *sv_serial = host_cfg_serial, *sv_pin = host_cfg_pin, *sv_iv = host_cfg_iv;
    host_cfg_serial = host_cfg_pin = host_cfg_iv = ""; // prebuilt: nothing in menuconfig
    nvs_reset();
    nvs_fail_part = 1;
    check("setting: no cfg partition (old partition table) -> fallback",
          !strcmp(clu_state_setting("wifi_ssid", "dflt"), "dflt") && nvs_part_inits == 1);
    nvs_fail_part = 0;
    check("setting: blank cfg partition -> fallback (also NULL)",
          !strcmp(clu_state_setting("wifi_ssid", "dflt"), "dflt") && !clu_state_setting("mqtt_uri", NULL) &&
              nvs_part_inits == 2);

    // first boot with a settings image: serial and PIN adopted, stored in the runtime NVS
    cfg_put("QWER5678", 221010155);
    nvs_handle_t h;
    nvs_open_from_partition("cfg", "cfg", NVS_READWRITE, &h);
    nvs_set_str(h, "wifi_ssid", "NvsNet");
    nvs_set_str(h, "mqtt_uri", "mqtts://broker.lan:8883");
    memset(&g_clu, 0, sizeof g_clu);
    clu_state_load();
    uint8_t k[16];
    clu_default_key((const uint8_t *)"QWER5678", k);
    check("provisioned: serial and PIN from the cfg partition, stored in nvs, factory key from the PIN",
          g_clu.sn == 221010155u && !strcmp(g_clu.pin, "QWER5678") && nvs_u32("clu", "sn") == 221010155u &&
              !strcmp(nvs_str("clu", "pin"), "QWER5678") && !memcmp(g_clu.project.key, k, 16) && nvs_part_inits == 2);
    check("setting: provisioned values win, other keys fall back",
          !strcmp(clu_state_setting("wifi_ssid", "dflt"), "NvsNet") &&
              !strcmp(clu_state_setting("mqtt_uri", NULL), "mqtts://broker.lan:8883") &&
              !strcmp(clu_state_setting("wifi_pass", "pw"), "pw") && !clu_state_setting("mqtt_user", NULL));
    int in_cfg = 0;
    for (int i = 0; i < nvs_n; i++) {
        in_cfg += !strncmp(nvs_db[i].ns, "cfg:", 4);
    }
    check("cfg partition only read: still just the 4 provisioned entries", in_cfg == 4);

    // OM sets a project key; reboot with the same settings image: key kept
    memset(g_clu.project.key, 0x5a, 16);
    clu_state_save_keys();
    memset(&g_clu, 0, sizeof g_clu);
    clu_state_load();
    check("provisioned: same serial / PIN on reboot -> OM's project key kept", g_clu.project.key[0] == 0x5a);

    // new settings image with another PIN: adopted, keys back to factory
    cfg_put("ZXCV9012", 0);
    clu_state_load();
    clu_default_key((const uint8_t *)"ZXCV9012", k);
    check("provisioned: changed PIN adopted, project key back to factory",
          !strcmp(g_clu.pin, "ZXCV9012") && g_clu.sn == 221010155u && !memcmp(g_clu.project.key, k, 16));

    // menuconfig wins over the cfg partition
    host_cfg_serial = "330017203";
    clu_state_load();
    check("provisioned: menuconfig serial wins over the cfg partition", g_clu.sn == 330017203u);
    host_cfg_serial = sv_serial, host_cfg_pin = sv_pin, host_cfg_iv = sv_iv;
}

static void test_state(void)
{
    nvs_reset();
    s_rnd = 0xf8;       // PIN draws hit the >= 252 rejection
    s_rnd_zero = 1;     // first serial draw is 0: drawn again
    clu_state_load();
    check_str("state: MAC from the WiFi STA address", g_clu.mac, "246F28ABCDEF");
    check("state: random serial (31-bit, non-zero) stored",
          g_clu.sn && !(g_clu.sn & 0x80000000u) && nvs_u32("clu", "sn") == g_clu.sn);
    bool pin_ok = strlen(g_clu.pin) == 8;
    for (int i = 0; i < 8; i++) {
        pin_ok = pin_ok && ((g_clu.pin[i] >= 'A' && g_clu.pin[i] <= 'Z') || (g_clu.pin[i] >= '0' && g_clu.pin[i] <= '9'));
    }
    check("state: random 8-char A-Z0-9 PIN stored", pin_ok && !strcmp(nvs_str("clu", "pin"), g_clu.pin));
    uint8_t k[16];
    clu_default_key((const uint8_t *)g_clu.pin, k);
    check("state: factory project key = default_key(PIN), stored with its IV",
          !memcmp(g_clu.project.key, k, 16) && nvs_peek("clu", "key") && !memcmp(nvs_peek("clu", "key")->val, k, 16) &&
              nvs_peek("clu", "iv") && !g_clu.has_temp);

    // second boot: nothing changes, nothing written
    clu_state_t before = g_clu;
    int commits = nvs_commits;
    memset(&g_clu, 0, sizeof g_clu);
    clu_state_load();
    check("state: reboot keeps serial, PIN and keys, no NVS write",
          g_clu.sn == before.sn && !strcmp(g_clu.pin, before.pin) && !memcmp(&g_clu.project, &before.project, sizeof g_clu.project) &&
              nvs_commits == commits);

    // keys set by OM survive a reboot
    memset(g_clu.project.key, 0x5a, 16);
    clu_state_save_keys();
    memset(&g_clu, 0, sizeof g_clu);
    clu_state_load();
    check("state: saved project key loaded at boot", g_clu.project.key[0] == 0x5a && g_clu.project.key[15] == 0x5a);

    // configured serial / PIN / IV: identity changes -> keys back to factory
    host_cfg_serial = "330017203";
    clu_state_load();
    clu_default_key((const uint8_t *)g_clu.pin, k);
    check("state: configured serial replaces the stored one, keys reset",
          g_clu.sn == 330017203u && nvs_u32("clu", "sn") == 330017203u && !memcmp(g_clu.project.key, k, 16));
    host_cfg_pin = "ABCD1234";
    clu_state_load();
    clu_default_key((const uint8_t *)"ABCD1234", k);
    check("state: configured PIN replaces the stored one, keys reset",
          !strcmp(g_clu.pin, "ABCD1234") && !strcmp(nvs_str("clu", "pin"), "ABCD1234") && !memcmp(g_clu.project.key, k, 16));
    host_cfg_iv = "000102030405060708090a0b0c0d0e0f";
    clu_state_load();
    check("state: configured factory IV used",
          g_clu.project.iv[0] == 0 && g_clu.project.iv[15] == 0x0f && !strcmp(nvs_str("clu", "civ"), host_cfg_iv));
    host_cfg_iv = "zz0102030405060708090a0b0c0d0e0f"; // not hex
    clu_state_load();
    check("state: non-hex configured IV -> random IV", g_clu.project.iv[0] != 0 || g_clu.project.iv[1] != 1);
    host_cfg_iv = "0102"; // wrong length
    clu_state_factory_keys();
    host_cfg_iv = "";
    host_cfg_serial = host_cfg_pin = "";
    nvs_fail_open = 1;
    clu_state_save_keys(); // no NVS: ignored
    nvs_fail_open = 0;
}

static void test_config_files(void)
{
    char a[512];
    snprintf(a, sizeof a, "%s/a", s_root);
    mkdir(a, 0775);
    g_clu.sn = 330017203;
    strcpy(g_clu.mac, "246F28ABCDEF");
    s_ndevs = 2;
    s_devs[0] = (tfbus_device_t){.sn = 252017533, .hw_type = 3, .hw_ver = 1, .fw_type = 3, .fw_api_ver = 4};
    s_devs[1] = (tfbus_device_t){.sn = 0x13ab6c81, .hw_type = 0x1e, .hw_ver = 3, .fw_type = 2, .fw_api_ver = 2,
                                 .have_ver = true, .ver = {2, 2, 4}};
    put_file("a/OM.LUA", "-- project\n");
    clu_state_write_config_files();
    check_str("CONFIG.JSON: CLU identity (lower-case MAC) + tfbusDevices", slurp("a/CONFIG.JSON"),
              "{\"sn\":330017203,\"mac\":\"24:6f:28:ab:cd:ef\",\"hwType\":19,\"hwVer\":1,\"fwType\":3,"
              "\"fwApiVer\":5,\"fwVer\":\"1.0.0\",\"status\":\"OK\",\"tfbusDevices\":["
              "{\"sn\":252017533,\"hwType\":3,\"hwVer\":1,\"fwType\":3,\"fwApiVer\":4,\"fwVer\":\"0.0.0\",\"status\":\"OK\"},"
              "{\"sn\":330001537,\"hwType\":30,\"hwVer\":3,\"fwType\":2,\"fwApiVer\":2,\"fwVer\":\"2.2.4\",\"status\":\"OK\"}]}");
    check_str("CONFIG.TXT: LF-only identity lines + one line per bus module", slurp("a/CONFIG.TXT"),
              "00000000\n13aba9b3\n24:6f:28:ab:cd:ef\n00000003\n00000005\n00000013\n00000001\n"
              "0f057b7d:03:03:04\n13ab6c81:1e:02:02\n");
    check_str("OM.LUA kept", slurp("a/OM.LUA"), "-- project\n");
    check_str("USER.LUA placeholder created", slurp("a/USER.LUA"), "");
    check("CLOUD-PUBLIC.CRT placeholder (skips Grenton cloud enrollment)",
          slurp("a/CLOUD-PUBLIC.CRT") && strstr(slurp("a/CLOUD-PUBLIC.CRT"), "no Grenton cloud"));
    put_file("a/CLOUD-PUBLIC.CRT", "real cert");
    clu_state_write_config_files();
    check_str("existing certificate kept", slurp("a/CLOUD-PUBLIC.CRT"), "real cert");

    // drive missing: nothing written, no crash
    char saved[256];
    strcpy(saved, s_root);
    strcat(s_root, "/missing");
    clu_state_write_config_files();
    strcpy(s_root, saved);
    check("config files: missing drive tolerated", 1);
}

// ---------------------------------------------------------------- clu_diag
static void test_diag(void)
{
    s_nmods = 3;
    s_mods[0] = (tfbus_module_t){.sn = 252017533, .hw_type = 3, .online = true, .fails_total = 2};
    s_mods[1] = (tfbus_module_t){.sn = 0x13ab6c81, .hw_type = 0x1e, .online = false, .fails_total = 40};
    s_mods[2] = (tfbus_module_t){.sn = 0x11111111, .hw_type = 0x99}; // never seen on the bus
    s_ndevs = 2;
    s_devs[0].hw_ver = 0; // unknown -> 1
    s_mqtt_up = true;
    check("diag: written", clu_diag_write("192.168.1.20") == 0);
    const char *d = slurp("m/DIAGNOSTIC_PACK.JSON");
    check("diag: CLU fields (sn, ip, cloudConn = MQTT link)",
          d && strstr(d, "\"sn\":\t330017203,") && strstr(d, "\"ipAddress\":\t\"192.168.1.20\"") &&
              strstr(d, "\"cloudConn\":\ttrue,") && strstr(d, "\"fwVer\":\t\"1.0.0\""));
    check("diag: module status / fails / identity from the last scan",
          d && strstr(d, "\"sn\":\t252017533,\n\t\t\t\"status\":\t\"OK\"") && strstr(d, "\"fails\":\t2,") &&
              strstr(d, "\"status\":\t\"DISCONNECTED\",\n\t\t\t\"voltage\":\t0,\n\t\t\t\"fails\":\t40,") &&
              strstr(d, "\"fwVer\":\t\"2.2.4\""));
    check("diag: module missing from the scan -> defaults (hwVer 1, fwType 2, fwApiVer 0)",
          d && strstr(d, "\"sn\":\t286331153,") && strstr(d, "\"hwType\":\t153,\n\t\t\t\"hwVer\":\t1,\n\t\t\t\"fwType\":\t2,\n\t\t\t\"fwApiVer\":\t0,"));
    check("diag: valid JSON shape (closing brackets)", d && strstr(d, "}]\n}"));
    s_mqtt_up = false;
    s_nmods = 0;
    clu_diag_write("1.2.3.4");
    check("diag: no modules, cloud down", strstr(slurp("m/DIAGNOSTIC_PACK.JSON"), "\"cloudConn\":\tfalse") &&
                                               strstr(slurp("m/DIAGNOSTIC_PACK.JSON"), "[]\n}"));

    char saved[256];
    strcpy(saved, s_root);
    strcat(s_root, "/missing");
    check("diag: drive missing -> 1 (mkdir failed)", clu_diag_write("1.2.3.4") == 1);
    strcpy(s_root, saved);
    char m[512];
    snprintf(m, sizeof m, "%s/m/DIAGNOSTIC_PACK.JSON", s_root);
    unlink(m);
    mkdir(m, 0775); // a directory where the file should go
    check("diag: file cannot be created -> 2", clu_diag_write("1.2.3.4") == 2);
    rmdir(m);
}

// ---------------------------------------------------------------- clu_sysinfo
static lua_State *L;

static int call(const char *fn, int nargs_int, lua_Integer arg)
{
    lua_getglobal(L, fn);
    if (nargs_int) {
        lua_pushinteger(L, arg);
    }
    lua_call(L, nargs_int, 1);
    return lua_type(L, -1);
}

static lua_Integer field_int(const char *k)
{
    lua_getfield(L, -1, k);
    lua_Integer v = lua_tointeger(L, -1);
    lua_pop(L, 1);
    return v;
}

static const char *field_str(const char *k)
{
    lua_getfield(L, -1, k);
    const char *v = lua_tostring(L, -1);
    lua_pop(L, 1);
    return v;
}

static void test_sysinfo(void)
{
    clu_sysinfo_init();
    check("sysinfo: Lua functions registered", s_ext && !strcmp(s_ext_name, "=sysinfo"));
    L = luaL_newstate();
    luaL_openlibs(L);
    for (const luaL_Reg *r = s_ext; r->name; r++) {
        lua_register(L, r->name, r->func);
    }

    s_nmods = 2;
    s_mods[0] = (tfbus_module_t){.sn = 1, .online = true, .fails_total = 3, .polls = 10, .addr = 1, .hw_type = 0x1e};
    s_mods[1] = (tfbus_module_t){.sn = 2, .online = false, .fails_total = 4};
    s_ipinfo.ip.addr = inet_addr("192.168.1.20");
    call("sys_info", 0, 0);
    check("sys_info: heap, PSRAM, reset reason, RSSI, IP",
          field_int("heap_free") == 123456 && field_int("heap_min") == 100000 && field_int("psram_free") == 8 << 20 &&
              !strcmp(field_str("reset_reason"), "power_on") && field_int("rssi") == -61 &&
              !strcmp(field_str("ip"), "192.168.1.20"));
    check("sys_info: bus totals", field_int("tfbus_total") == 2 && field_int("tfbus_online") == 1 && field_int("tfbus_fails") == 7);
    lua_pop(L, 1);

    static const struct { esp_reset_reason_t r; const char *s; } reasons[] = {
        {ESP_RST_EXT, "external"}, {ESP_RST_SW, "software"}, {ESP_RST_PANIC, "panic"},
        {ESP_RST_INT_WDT, "interrupt_watchdog"}, {ESP_RST_TASK_WDT, "task_watchdog"}, {ESP_RST_WDT, "watchdog"},
        {ESP_RST_DEEPSLEEP, "deep_sleep"}, {ESP_RST_BROWNOUT, "brownout"}, {ESP_RST_SDIO, "sdio"},
        {ESP_RST_UNKNOWN, "unknown"},
    };
    bool all = true;
    for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++) {
        s_reason = reasons[i].r;
        call("sys_info", 0, 0);
        all = all && !strcmp(field_str("reset_reason"), reasons[i].s);
        lua_pop(L, 1);
    }
    check("sys_info: every reset reason named", all);

    s_rssi_ok = 0;
    s_netif_present = false;
    call("sys_info", 0, 0);
    lua_getfield(L, -1, "rssi");
    lua_getfield(L, -2, "ip");
    check("sys_info: no RSSI / no interface -> fields absent", lua_isnil(L, -1) && lua_isnil(L, -2));
    lua_pop(L, 3);
    s_netif_present = true;
    s_ipinfo.ip.addr = 0;
    call("sys_info", 0, 0);
    lua_getfield(L, -1, "ip");
    check("sys_info: no address yet -> ip absent", lua_isnil(L, -1));
    lua_pop(L, 2);

    call("tfbus_module", 1, 1);
    lua_getfield(L, -1, "online");
    check("tfbus_module(sn): online / fails / polls / addr / hwType",
          lua_toboolean(L, -1) && (lua_pop(L, 1), field_int("fails") == 3 && field_int("polls") == 10 &&
                                                       field_int("addr") == 1 && field_int("hw_type") == 0x1e));
    lua_pop(L, 1);
    check("tfbus_module(unknown sn) -> nil", call("tfbus_module", 1, 99) == LUA_TNIL);
    lua_pop(L, 1);

    s_timer_fail = 1;
    lua_getglobal(L, "sys_restart");
    lua_call(L, 0, 0);
    check("sys_restart: no timer available -> nothing", s_shutdown_hooks == 0 && s_timer_starts == 0);
    s_timer_fail = 0;
    lua_getglobal(L, "sys_restart");
    lua_call(L, 0, 0);
    lua_getglobal(L, "sys_restart");
    lua_call(L, 0, 0);
    check("sys_restart: storage flushed, reboot in 1 s (timer reused)",
          s_shutdown_hooks == 2 && s_timer_starts == 2 && s_timer_us == 1000000);
    s_timer_args.callback(s_timer_args.arg);
    check("sys_restart: timer -> esp_restart()", s_restarts == 1);
    lua_close(L);
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    return remove(path);
}

int main(void)
{
    snprintf(s_root, sizeof s_root, "/tmp/clu_test_sys_XXXXXX");
    if (!mkdtemp(s_root)) {
        perror("mkdtemp");
        return 1;
    }
    test_net();
    test_time();
    test_time_rtc();
    test_provisioned();
    test_state();
    test_config_files();
    test_diag();
    test_sysinfo();

    // remove the temporary drive (only ever the mkdtemp directory created above)
    if (strncmp(s_root, "/tmp/clu_test_sys_", 18) == 0) {
        nftw(s_root, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
    }
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
