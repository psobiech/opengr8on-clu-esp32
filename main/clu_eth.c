/**
 * @file clu_eth.c
 * @brief Ethernet interface: ESP32 internal EMAC + LAN8720A RMII PHY (Grenton GATE hardware).
 *
 * Runs alongside the WiFi STA when both are enabled. Its own IP config (DHCP / static from OM) lives
 * in clu_net (CLU_NET_ETH). Ethernet gets a higher route priority than WiFi, so the default route
 * (and DNS / SNTP / MQTT traffic) goes over the wire while it has an address.
 */
#include "clu_eth.h"

#include "sdkconfig.h"

#if CONFIG_CLU_ETH

#include "clu_net.h"
#include "esp_eth.h"
#include "esp_eth_phy_lan87xx.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"

static const char *TAG = "clu_eth";

// Above WIFI_STA (100): wired traffic wins the default route.
#define ETH_ROUTE_PRIO 128

#if CONFIG_CLU_ETH_CLK_EXT_IN_GPIO0
#define ETH_CLK_MODE EMAC_CLK_EXT_IN
#define ETH_CLK_GPIO 0
#elif CONFIG_CLU_ETH_CLK_OUT_GPIO0
#define ETH_CLK_MODE EMAC_CLK_OUT
#define ETH_CLK_GPIO 0
#elif CONFIG_CLU_ETH_CLK_OUT_GPIO16
#define ETH_CLK_MODE EMAC_CLK_OUT
#define ETH_CLK_GPIO 16
#else
#define ETH_CLK_MODE EMAC_CLK_OUT
#define ETH_CLK_GPIO 17
#endif

/** @brief Ethernet driver events: link state for the log. */
static void on_eth(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED: {
        esp_eth_handle_t h = *(esp_eth_handle_t *)data;
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        esp_eth_ioctl(h, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(h, ETH_CMD_G_DUPLEX_MODE, &duplex);
        ESP_LOGI(TAG, "link up, %s Mbps %s duplex", speed == ETH_SPEED_100M ? "100" : "10",
                 duplex == ETH_DUPLEX_FULL ? "full" : "half");
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED: ESP_LOGW(TAG, "link down"); break;
    default: break;
    }
}

void clu_eth_start(void)
{
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.route_prio = ETH_ROUTE_PRIO;
    esp_netif_config_t ncfg = {.base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
    esp_netif_t *netif = esp_netif_new(&ncfg);

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = CONFIG_CLU_ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = CONFIG_CLU_ETH_MDIO_GPIO;
    emac_cfg.clock_config.rmii.clock_mode = ETH_CLK_MODE;
    emac_cfg.clock_config.rmii.clock_gpio = ETH_CLK_GPIO;

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = CONFIG_CLU_ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = CONFIG_CLU_ETH_PHY_RST_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    esp_eth_phy_t *phy = mac ? esp_eth_phy_new_lan87xx(&phy_cfg) : NULL;
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    esp_err_t err = phy ? esp_eth_driver_install(&cfg, &eth) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        // No PHY answering (wrong address / clock) ends up here: run on without Ethernet.
        ESP_LOGE(TAG, "driver install failed: %s (PHY addr %d, MDC %d, MDIO %d, clock GPIO%d %s)",
                 esp_err_to_name(err), CONFIG_CLU_ETH_PHY_ADDR, CONFIG_CLU_ETH_MDC_GPIO, CONFIG_CLU_ETH_MDIO_GPIO,
                 ETH_CLK_GPIO, ETH_CLK_MODE == EMAC_CLK_EXT_IN ? "in" : "out");
        if (phy) {
            phy->del(phy);
        }
        if (mac) {
            mac->del(mac);
        }
        esp_netif_destroy(netif);
        return;
    }
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL));
    clu_net_init(CLU_NET_ETH, netif); // after attach: DHCP / static needs the started netif driver
    uint8_t m[6];
    esp_eth_ioctl(eth, ETH_CMD_G_MAC_ADDR, m);
    ESP_LOGI(TAG, "EMAC + LAN8720A up, MAC " MACSTR, MAC2STR(m));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}

#else

void clu_eth_start(void) {}

#endif // CONFIG_CLU_ETH
