/**
 * @file clu_net.h
 * @brief Persistent IP configuration of the WiFi STA interface (DHCP / static IP, DNS).
 */
#pragma once

#include <stdbool.h>
#include "esp_netif.h"

/**
 * @brief Load persisted IP config and apply it to the STA netif.
 *
 * Call before esp_wifi_start().
 *
 * @param sta STA network interface.
 */
void clu_net_init(esp_netif_t *sta);

/** @brief Re-apply the current config (static IP or DHCP). */
void clu_net_apply(void);

/**
 * @brief Persist a static IP from req_set_clu_ip. Applied by clu_net_apply().
 *
 * The request carries no netmask: /24 is assumed, or no mask (everything on-link) if the gateway
 * lies outside that /24.
 *
 * @param ip Dotted IPv4 address; "0.0.0.0" switches back to DHCP.
 * @param gw Dotted IPv4 gateway.
 * @return true if the address was valid and saved.
 */
bool clu_net_set_static(const char *ip, const char *gw);

/**
 * @brief Set a DNS server (CLU features 25 PrimaryDNS / 26 SecondaryDNS).
 *
 * Used with a static IP.
 *
 * @param idx  0 = primary, 1 = secondary.
 * @param addr Dotted IPv4 address.
 */
void clu_net_set_dns(int idx, const char *addr);
