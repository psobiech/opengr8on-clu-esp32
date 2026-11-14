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
static void put_u32(nvs_handle_t nvs_handle, const char *key, uint32_t value)
{
    nvs_set_u32(nvs_handle, key, value);
}

/** @brief Persist the static IP config of interface @p interface_index to NVS. */
static void save(clu_net_if_t interface_index)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(s_ns[interface_index], NVS_READWRITE, &nvs_handle) != ESP_OK) {
        return;
    }
    put_u32(nvs_handle, "ip", s_if[interface_index].ip);
    put_u32(nvs_handle, "gw", s_if[interface_index].gw);
    put_u32(nvs_handle, "mask", s_if[interface_index].mask);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

/** @brief Persist the DNS servers to NVS. */
static void save_dns(void)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NS_DNS, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        return;
    }
    put_u32(nvs_handle, "dns1", s_dns[0]);
    put_u32(nvs_handle, "dns2", s_dns[1]);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

/** @brief Apply DNS server @p idx (0 = main, 1 = backup) to @p netif; no-op for address 0. */
static void set_dns(esp_netif_t *netif, int idx, uint32_t addr)
{
    if (!netif || !addr) {
        return;
    }
    esp_netif_dns_info_t dns_info = {.ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4.addr = addr};
    esp_netif_set_dns_info(netif, idx == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &dns_info);
}

void clu_net_init(clu_net_if_t interface_index, esp_netif_t *netif)
{
    net_if_t *interface_config = &s_if[interface_index];
    interface_config->netif = netif;
    nvs_handle_t nvs_handle;
    if (nvs_open(s_ns[interface_index], NVS_READONLY, &nvs_handle) == ESP_OK) {
        nvs_get_u32(nvs_handle, "ip", &interface_config->ip);
        nvs_get_u32(nvs_handle, "gw", &interface_config->gw);
        nvs_get_u32(nvs_handle, "mask", &interface_config->mask);
        nvs_close(nvs_handle);
    }
    if (nvs_open(NVS_NS_DNS, NVS_READONLY, &nvs_handle) == ESP_OK) {
        nvs_get_u32(nvs_handle, "dns1", &s_dns[0]);
        nvs_get_u32(nvs_handle, "dns2", &s_dns[1]);
        nvs_close(nvs_handle);
    }
    clu_net_apply(interface_index);
}

void clu_net_apply(clu_net_if_t interface_index)
{
    net_if_t *interface_config = &s_if[interface_index];
    if (!interface_config->netif) {
        return;
    }
    if (!interface_config->ip) {
        esp_netif_dhcpc_start(interface_config->netif); // ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED is fine
        ESP_LOGI(TAG, "%s: DHCP", s_name[interface_index]);
        return;
    }
    esp_netif_dhcpc_stop(interface_config->netif);
    esp_netif_ip_info_t ip_info = {.ip.addr = interface_config->ip, .gw.addr = interface_config->gw, .netmask.addr = interface_config->mask};
    esp_err_t err = esp_netif_set_ip_info(interface_config->netif, &ip_info);
    set_dns(interface_config->netif, 0, s_dns[0] ? s_dns[0] : interface_config->gw);
    set_dns(interface_config->netif, 1, s_dns[1]);
    ESP_LOGI(TAG, "%s: static " IPSTR " gw " IPSTR " mask " IPSTR " (%s)", s_name[interface_index], IP2STR(&ip_info.ip),
             IP2STR(&ip_info.gw), IP2STR(&ip_info.netmask), esp_err_to_name(err));
}

bool clu_net_set_static(clu_net_if_t interface_index, const char *ip, const char *gateway_address)
{
    net_if_t *interface_config = &s_if[interface_index];
    struct in_addr ip_address, gateway = {0};
    if (!inet_aton(ip, &ip_address)) {
        return false;
    }
    if (ip_address.s_addr == 0) { // 0.0.0.0 -> back to DHCP (not a Grenton convention)
        interface_config->ip = interface_config->gw = interface_config->mask = 0;
        save(interface_index);
        return true;
    }
    if (gateway_address && *gateway_address && !inet_aton(gateway_address, &gateway)) {
        return false;
    }
    interface_config->ip = ip_address.s_addr;
    interface_config->gw = gateway.s_addr;
    interface_config->mask = htonl(0xffffff00); // req_set_clu_ip carries no netmask: assume /24
    if (interface_config->gw && ((interface_config->gw ^ interface_config->ip) & interface_config->mask)) {
        interface_config->mask = 0; // gateway outside /24: let lwIP route everything on-link
    }
    save(interface_index);
    return true;
}

void clu_net_set_dns(int idx, const char *addr)
{
    struct in_addr dns_address;
    if (idx < 0 || idx > 1 || !addr || !inet_aton(addr, &dns_address) || s_dns[idx] == dns_address.s_addr) {
        return;
    }
    s_dns[idx] = dns_address.s_addr;
    save_dns();
    for (int i = 0; i < CLU_NET_IF_COUNT; i++) {
        if (s_if[i].ip) {
            set_dns(s_if[i].netif, idx, dns_address.s_addr);
        }
    }
}

uint32_t clu_net_ip(clu_net_if_t interface_index)
{
    esp_netif_ip_info_t ip_info;
    if (!s_if[interface_index].netif || esp_netif_get_ip_info(s_if[interface_index].netif, &ip_info) != ESP_OK) {
        return 0;
    }
    return ip_info.ip.addr;
}

// Preference when the subnet does not decide: wired first.
static const clu_net_if_t s_order[CLU_NET_IF_COUNT] = {CLU_NET_ETH, CLU_NET_WIFI};

clu_net_if_t clu_net_if_for(uint32_t peer_be)
{
    esp_netif_ip_info_t ip_info;
    if (peer_be) {
        for (int k = 0; k < CLU_NET_IF_COUNT; k++) {
            const net_if_t *interface_config = &s_if[s_order[k]];
            if (interface_config->netif && esp_netif_get_ip_info(interface_config->netif, &ip_info) == ESP_OK && ip_info.ip.addr &&
                ((ip_info.ip.addr ^ peer_be) & ip_info.netmask.addr) == 0) {
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
