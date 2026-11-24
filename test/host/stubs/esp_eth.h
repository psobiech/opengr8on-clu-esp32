// Host stub: the ESP-IDF Ethernet driver API used by clu_eth.c (internal EMAC + PHY), implemented
// by the test. MAC / PHY objects are structs with a del callback, as in the IDF.
#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"
typedef struct esp_eth_mac_s esp_eth_mac_t;
struct esp_eth_mac_s {
    esp_err_t (*del)(esp_eth_mac_t *mac);
};
typedef struct esp_eth_phy_s esp_eth_phy_t;
struct esp_eth_phy_s {
    esp_err_t (*del)(esp_eth_phy_t *phy);
};
typedef void *esp_eth_handle_t;
typedef void *esp_eth_netif_glue_handle_t;
typedef struct {
    uint32_t rx_task_stack_size;
} eth_mac_config_t;
#define ETH_MAC_DEFAULT_CONFIG() {.rx_task_stack_size = 2048}
typedef struct {
    int32_t phy_addr;
    int reset_gpio_num;
} eth_phy_config_t;
#define ETH_PHY_DEFAULT_CONFIG() {.phy_addr = -1, .reset_gpio_num = 5}
typedef struct {
    esp_eth_mac_t *mac;
    esp_eth_phy_t *phy;
} esp_eth_config_t;
#define ETH_DEFAULT_CONFIG(emac, ephy) {.mac = (emac), .phy = (ephy)}
typedef enum { ETH_CMD_G_MAC_ADDR, ETH_CMD_S_MAC_ADDR, ETH_CMD_G_SPEED, ETH_CMD_G_DUPLEX_MODE } esp_eth_io_cmd_t;
typedef enum { ETH_SPEED_10M, ETH_SPEED_100M } eth_speed_t;
typedef enum { ETH_DUPLEX_HALF, ETH_DUPLEX_FULL } eth_duplex_t;
typedef enum { ETHERNET_EVENT_START, ETHERNET_EVENT_STOP, ETHERNET_EVENT_CONNECTED, ETHERNET_EVENT_DISCONNECTED } eth_event_t;
extern const char *const ETH_EVENT;
esp_err_t esp_eth_driver_install(const esp_eth_config_t *config, esp_eth_handle_t *out_hdl);
esp_err_t esp_eth_ioctl(esp_eth_handle_t hdl, esp_eth_io_cmd_t cmd, void *data);
esp_eth_netif_glue_handle_t esp_eth_new_netif_glue(esp_eth_handle_t eth_hdl);
esp_err_t esp_eth_start(esp_eth_handle_t hdl);
// Internal EMAC (RMII)
typedef enum { EMAC_CLK_DEFAULT, EMAC_CLK_EXT_IN, EMAC_CLK_OUT } emac_rmii_clock_mode_t;
typedef struct {
    struct {
        int mdc_num;
        int mdio_num;
    } smi_gpio;
    struct {
        struct {
            emac_rmii_clock_mode_t clock_mode;
            int clock_gpio;
        } rmii;
    } clock_config;
} eth_esp32_emac_config_t;
#define ETH_ESP32_EMAC_DEFAULT_CONFIG() {.smi_gpio = {23, 18}}
esp_eth_mac_t *esp_eth_mac_new_esp32(const eth_esp32_emac_config_t *esp32_config, const eth_mac_config_t *config);
esp_eth_phy_t *esp_eth_phy_new_generic(const eth_phy_config_t *config);
