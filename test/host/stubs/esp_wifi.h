#pragma once
#include "esp_err.h"
esp_err_t esp_wifi_sta_get_rssi(int *rssi);

// ---- types and calls used by main.c (implemented by test_main.c)
#include <stdbool.h>
#include <stdint.h>
#include "esp_event.h"
#include "esp_netif.h"
typedef struct { uint8_t *ssid; bool show_hidden; } wifi_scan_config_t;
typedef struct {
    uint8_t bssid[6];
    uint8_t primary;
    int8_t rssi;
    int authmode, pairwise_cipher, group_cipher;
    unsigned phy_11b : 1, phy_11g : 1, phy_11n : 1, phy_11ax : 1;
} wifi_ap_record_t;
typedef struct { uint8_t ssid[32]; uint8_t ssid_len; uint8_t channel; int authmode; } wifi_event_sta_connected_t;
typedef struct { uint8_t reason; int8_t rssi; } wifi_event_sta_disconnected_t;
typedef struct { esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
typedef struct { int dummy; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() {0}
typedef struct {
    struct {
        uint8_t ssid[32];
        uint8_t password[64];
        struct { int authmode; } threshold;
        int sae_pwe_h2e;
        struct { bool capable; } pmf_cfg;
        uint8_t bssid[6];
        bool bssid_set;
    } sta;
} wifi_config_t;
enum { WIFI_AUTH_WPA2_PSK = 3, WIFI_AUTH_WPA3_PSK = 6 };
enum { WPA3_SAE_PWE_BOTH = 2 };
typedef enum { WIFI_MODE_STA = 1 } wifi_mode_t;
typedef enum { WIFI_IF_STA = 0 } wifi_interface_t;
typedef enum { WIFI_PS_NONE = 0 } wifi_ps_type_t;
enum { WIFI_EVENT_SCAN_DONE = 1, WIFI_EVENT_STA_START = 2, WIFI_EVENT_STA_CONNECTED = 4, WIFI_EVENT_STA_DISCONNECTED = 5 };
enum { IP_EVENT_STA_GOT_IP = 0 };
extern esp_event_base_t const WIFI_EVENT, IP_EVENT;
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *c, bool block);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *ap);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_init(const wifi_init_config_t *c);
esp_err_t esp_wifi_set_mode(wifi_mode_t m);
esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c);
esp_err_t esp_wifi_get_config(wifi_interface_t i, wifi_config_t *c);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t t);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_set_max_tx_power(int8_t p);
esp_err_t esp_wifi_get_max_tx_power(int8_t *p);
