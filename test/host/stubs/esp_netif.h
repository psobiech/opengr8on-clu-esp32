// Host stub: esp_netif calls used by clu_net.c / clu_sysinfo.c, implemented by the test.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct { esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
enum { IP_EVENT_STA_GOT_IP = 0, IP_EVENT_STA_LOST_IP = 1, IP_EVENT_ETH_GOT_IP = 4, IP_EVENT_ETH_LOST_IP = 5 };
extern const char *const IP_EVENT;
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
// Interface creation (clu_eth.c)
typedef struct {
    int route_prio;
} esp_netif_inherent_config_t;
#define ESP_NETIF_INHERENT_DEFAULT_ETH() {.route_prio = 50}
#define ESP_NETIF_NETSTACK_DEFAULT_ETH ((const void *)"eth")
typedef struct {
    const esp_netif_inherent_config_t *base;
    const void *stack;
} esp_netif_config_t;
esp_netif_t *esp_netif_new(const esp_netif_config_t *config);
void esp_netif_destroy(esp_netif_t *netif);
esp_err_t esp_netif_attach(esp_netif_t *netif, void *driver_handle);
