/**
 * @file clu_net.h
 * @brief Persistent IP configuration of the network interfaces (WiFi STA, Ethernet): DHCP / static
 * IP per interface, DNS.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

/** @brief Network interfaces with their own IP configuration. */
typedef enum {
    CLU_NET_WIFI, /**< WiFi station (NVS "clu_net") */
    CLU_NET_ETH,  /**< Ethernet (NVS "clu_net_eth") */
    CLU_NET_IF_COUNT,
} clu_net_if_t;

/**
 * @brief Load the persisted IP config of interface @p interface_index and apply it to @p netif.
 *
 * Call before the interface is started.
 *
 * @param interface_index Interface.
 * @param netif           Its network interface.
 */
void clu_net_init(clu_net_if_t interface_index, esp_netif_t *netif);

/** @brief Re-apply the current config (static IP or DHCP) of interface @p interface_index. */
void clu_net_apply(clu_net_if_t interface_index);

/**
 * @brief Persist a static IP of interface @p interface_index from req_set_clu_ip. Applied by clu_net_apply().
 *
 * The request carries no netmask: /24 is assumed, or no mask (everything on-link) if the gateway
 * lies outside that /24.
 *
 * @param interface_index Interface.
 * @param ip              Dotted IPv4 address; "0.0.0.0" switches back to DHCP.
 * @param gateway_address Dotted IPv4 gateway.
 * @return true if the address was valid and saved.
 */
bool clu_net_set_static(clu_net_if_t interface_index, const char *ip, const char *gateway_address);

/**
 * @brief Set a DNS server (CLU features 25 PrimaryDNS / 26 SecondaryDNS).
 *
 * Shared by the interfaces; used by those with a static IP.
 *
 * @param idx  0 = primary, 1 = secondary.
 * @param addr Dotted IPv4 address.
 */
void clu_net_set_dns(int idx, const char *addr);

/**
 * @brief Interface a peer is reached through.
 *
 * The started interface whose subnet holds @p peer_be; otherwise the first one with an address
 * (Ethernet before WiFi); otherwise the first started one (WiFi if none is).
 *
 * @param peer_be Peer IPv4 address in network byte order; 0 = no peer (skip the subnet match).
 */
clu_net_if_t clu_net_if_for(uint32_t peer_be);

/**
 * @brief Current IPv4 address of interface @p interface_index.
 *
 * @return Address in network byte order, 0 if the interface is not started or has none.
 */
uint32_t clu_net_ip(clu_net_if_t interface_index);

/**
 * @brief Our address towards @p peer_be: clu_net_ip(clu_net_if_for(peer_be)).
 *
 * @param peer_be Peer IPv4 address in network byte order; 0 = no peer.
 */
uint32_t clu_net_local_ip(uint32_t peer_be);
