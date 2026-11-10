// Host stub: esp_netif calls used by clu_net.c / clu_sysinfo.c, implemented by the test.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
#define ESP_IPADDR_TYPE_V4 0
typedef struct {
    struct {
        union { esp_ip4_addr_t ip4; } u_addr;
        uint8_t type;
    } ip;
} esp_netif_dns_info_t;
typedef enum { ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP } esp_netif_dns_type_t;
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int)((a)->addr & 0xff), (int)(((a)->addr >> 8) & 0xff), (int)(((a)->addr >> 16) & 0xff), \
                  (int)(((a)->addr >> 24) & 0xff)
esp_err_t esp_netif_dhcpc_start(esp_netif_t *n);
esp_err_t esp_netif_dhcpc_stop(esp_netif_t *n);
esp_err_t esp_netif_set_ip_info(esp_netif_t *n, const esp_netif_ip_info_t *ii);
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ii);
esp_err_t esp_netif_set_dns_info(esp_netif_t *n, esp_netif_dns_type_t t, esp_netif_dns_info_t *d);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key);
char *esp_ip4addr_ntoa(const esp_ip4_addr_t *a, char *buf, int len);
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
