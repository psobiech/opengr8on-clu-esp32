/**
 * @file clu_net.c
 * @brief STA IPv4 configuration: DHCP (default) or static IP assigned by OM with req_set_clu_ip.
 *
 * Static config is persisted in NVS ("clu_net": ip, gw, mask, dns1, dns2 as u32 network order).
 */
#include "clu_net.h"

#include <string.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "nvs.h"

static const char *TAG = "clu_net";
#define NVS_NS "clu_net"

static esp_netif_t *s_netif;
static uint32_t s_ip, s_gw, s_mask, s_dns[2]; // network byte order, s_ip == 0 -> DHCP

/** @brief Write one u32 NVS key (errors ignored). */
static void put_u32(nvs_handle_t h, const char *k, uint32_t v)
{
    nvs_set_u32(h, k, v);
}

/** @brief Persist the current IP/DNS config to NVS. */
static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    put_u32(h, "ip", s_ip);
    put_u32(h, "gw", s_gw);
    put_u32(h, "mask", s_mask);
    put_u32(h, "dns1", s_dns[0]);
    put_u32(h, "dns2", s_dns[1]);
    nvs_commit(h);
    nvs_close(h);
}

/** @brief Apply DNS server @p i (0 = main, 1 = backup) to the netif; no-op for address 0. */
static void set_dns(int i, uint32_t addr)
{
    if (!s_netif || !addr) {
        return;
    }
    esp_netif_dns_info_t d = {.ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4.addr = addr};
    esp_netif_set_dns_info(s_netif, i == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &d);
}

void clu_net_init(esp_netif_t *sta)
{
    s_netif = sta;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "ip", &s_ip);
        nvs_get_u32(h, "gw", &s_gw);
        nvs_get_u32(h, "mask", &s_mask);
        nvs_get_u32(h, "dns1", &s_dns[0]);
        nvs_get_u32(h, "dns2", &s_dns[1]);
        nvs_close(h);
    }
    clu_net_apply();
}

void clu_net_apply(void)
{
    if (!s_netif) {
        return;
    }
    if (!s_ip) {
        esp_netif_dhcpc_start(s_netif); // ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED is fine
        ESP_LOGI(TAG, "DHCP");
        return;
    }
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_ip_info_t ii = {.ip.addr = s_ip, .gw.addr = s_gw, .netmask.addr = s_mask};
    esp_err_t err = esp_netif_set_ip_info(s_netif, &ii);
    set_dns(0, s_dns[0] ? s_dns[0] : s_gw);
    set_dns(1, s_dns[1]);
    ESP_LOGI(TAG, "static " IPSTR " gw " IPSTR " mask " IPSTR " (%s)", IP2STR(&ii.ip), IP2STR(&ii.gw),
             IP2STR(&ii.netmask), esp_err_to_name(err));
}

bool clu_net_set_static(const char *ip, const char *gw)
{
    struct in_addr a, g = {0};
    if (!inet_aton(ip, &a)) {
        return false;
    }
    if (a.s_addr == 0) { // 0.0.0.0 -> back to DHCP (not a Grenton convention)
        s_ip = s_gw = s_mask = 0;
        save();
        return true;
    }
    if (gw && *gw && !inet_aton(gw, &g)) {
        return false;
    }
    s_ip = a.s_addr;
    s_gw = g.s_addr;
    s_mask = htonl(0xffffff00); // req_set_clu_ip carries no netmask: assume /24
    if (s_gw && ((s_gw ^ s_ip) & s_mask)) {
        s_mask = 0; // gateway outside /24: let lwIP route everything on-link
    }
    save();
    return true;
}

void clu_net_set_dns(int idx, const char *addr)
{
    struct in_addr a;
    if (idx < 0 || idx > 1 || !addr || !inet_aton(addr, &a) || s_dns[idx] == a.s_addr) {
        return;
    }
    s_dns[idx] = a.s_addr;
    save();
    if (s_ip) {
        set_dns(idx, a.s_addr);
    }
}
