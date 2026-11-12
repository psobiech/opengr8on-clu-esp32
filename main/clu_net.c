/**
 * @file clu_net.c
 * @brief Per-interface IPv4 configuration: DHCP (default) or static IP assigned by OM with
 * req_set_clu_ip.
 *
 * Static config is persisted in NVS per interface (ip, gw, mask as u32 network order): "clu_net" for
 * WiFi (the namespace of the WiFi-only firmware, kept), "clu_net_eth" for Ethernet. The DNS servers
 * (dns1, dns2) are shared and live in "clu_net".
 */
#include "clu_net.h"

#include <string.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "nvs.h"

static const char *TAG = "clu_net";
#define NVS_NS_DNS "clu_net"

typedef struct {
    esp_netif_t *netif;
    uint32_t ip, gw, mask; // network byte order, ip == 0 -> DHCP
} net_if_t;

static const char *const s_ns[CLU_NET_IF_COUNT] = {"clu_net", "clu_net_eth"};
static const char *const s_name[CLU_NET_IF_COUNT] = {"wifi", "eth"};
static net_if_t s_if[CLU_NET_IF_COUNT];
static uint32_t s_dns[2]; // network byte order

/** @brief Write one u32 NVS key (errors ignored). */
static void put_u32(nvs_handle_t h, const char *k, uint32_t v)
{
    nvs_set_u32(h, k, v);
}

/** @brief Persist the static IP config of interface @p i to NVS. */
static void save(clu_net_if_t i)
{
    nvs_handle_t h;
    if (nvs_open(s_ns[i], NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    put_u32(h, "ip", s_if[i].ip);
    put_u32(h, "gw", s_if[i].gw);
    put_u32(h, "mask", s_if[i].mask);
    nvs_commit(h);
    nvs_close(h);
}

/** @brief Persist the DNS servers to NVS. */
static void save_dns(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_DNS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    put_u32(h, "dns1", s_dns[0]);
    put_u32(h, "dns2", s_dns[1]);
    nvs_commit(h);
    nvs_close(h);
}

/** @brief Apply DNS server @p idx (0 = main, 1 = backup) to @p netif; no-op for address 0. */
static void set_dns(esp_netif_t *netif, int idx, uint32_t addr)
{
    if (!netif || !addr) {
        return;
    }
    esp_netif_dns_info_t d = {.ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4.addr = addr};
    esp_netif_set_dns_info(netif, idx == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &d);
}

void clu_net_init(clu_net_if_t i, esp_netif_t *netif)
{
    net_if_t *n = &s_if[i];
    n->netif = netif;
    nvs_handle_t h;
    if (nvs_open(s_ns[i], NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "ip", &n->ip);
        nvs_get_u32(h, "gw", &n->gw);
        nvs_get_u32(h, "mask", &n->mask);
        nvs_close(h);
    }
    if (nvs_open(NVS_NS_DNS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "dns1", &s_dns[0]);
        nvs_get_u32(h, "dns2", &s_dns[1]);
        nvs_close(h);
    }
    clu_net_apply(i);
}

void clu_net_apply(clu_net_if_t i)
{
    net_if_t *n = &s_if[i];
    if (!n->netif) {
        return;
    }
    if (!n->ip) {
        esp_netif_dhcpc_start(n->netif); // ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED is fine
        ESP_LOGI(TAG, "%s: DHCP", s_name[i]);
        return;
    }
    esp_netif_dhcpc_stop(n->netif);
    esp_netif_ip_info_t ii = {.ip.addr = n->ip, .gw.addr = n->gw, .netmask.addr = n->mask};
    esp_err_t err = esp_netif_set_ip_info(n->netif, &ii);
    set_dns(n->netif, 0, s_dns[0] ? s_dns[0] : n->gw);
    set_dns(n->netif, 1, s_dns[1]);
    ESP_LOGI(TAG, "%s: static " IPSTR " gw " IPSTR " mask " IPSTR " (%s)", s_name[i], IP2STR(&ii.ip),
             IP2STR(&ii.gw), IP2STR(&ii.netmask), esp_err_to_name(err));
}

bool clu_net_set_static(clu_net_if_t i, const char *ip, const char *gw)
{
    net_if_t *n = &s_if[i];
    struct in_addr a, g = {0};
    if (!inet_aton(ip, &a)) {
        return false;
    }
    if (a.s_addr == 0) { // 0.0.0.0 -> back to DHCP (not a Grenton convention)
        n->ip = n->gw = n->mask = 0;
        save(i);
        return true;
    }
    if (gw && *gw && !inet_aton(gw, &g)) {
        return false;
    }
    n->ip = a.s_addr;
    n->gw = g.s_addr;
    n->mask = htonl(0xffffff00); // req_set_clu_ip carries no netmask: assume /24
    if (n->gw && ((n->gw ^ n->ip) & n->mask)) {
        n->mask = 0; // gateway outside /24: let lwIP route everything on-link
    }
    save(i);
    return true;
}

void clu_net_set_dns(int idx, const char *addr)
{
    struct in_addr a;
    if (idx < 0 || idx > 1 || !addr || !inet_aton(addr, &a) || s_dns[idx] == a.s_addr) {
        return;
    }
    s_dns[idx] = a.s_addr;
    save_dns();
    for (int i = 0; i < CLU_NET_IF_COUNT; i++) {
        if (s_if[i].ip) {
            set_dns(s_if[i].netif, idx, a.s_addr);
        }
    }
}

uint32_t clu_net_ip(clu_net_if_t i)
{
    esp_netif_ip_info_t ii;
    if (!s_if[i].netif || esp_netif_get_ip_info(s_if[i].netif, &ii) != ESP_OK) {
        return 0;
    }
    return ii.ip.addr;
}

// Preference when the subnet does not decide: wired first.
static const clu_net_if_t s_order[CLU_NET_IF_COUNT] = {CLU_NET_ETH, CLU_NET_WIFI};

clu_net_if_t clu_net_if_for(uint32_t peer_be)
{
    esp_netif_ip_info_t ii;
    if (peer_be) {
        for (int k = 0; k < CLU_NET_IF_COUNT; k++) {
            const net_if_t *n = &s_if[s_order[k]];
            if (n->netif && esp_netif_get_ip_info(n->netif, &ii) == ESP_OK && ii.ip.addr &&
                ((ii.ip.addr ^ peer_be) & ii.netmask.addr) == 0) {
                return s_order[k];
            }
        }
    }
    for (int k = 0; k < CLU_NET_IF_COUNT; k++) {
        if (clu_net_ip(s_order[k])) {
            return s_order[k];
        }
    }
    for (int k = 0; k < CLU_NET_IF_COUNT; k++) {
        if (s_if[s_order[k]].netif) {
            return s_order[k];
        }
    }
    return CLU_NET_WIFI;
}

uint32_t clu_net_local_ip(uint32_t peer_be)
{
    return clu_net_ip(clu_net_if_for(peer_be));
}
