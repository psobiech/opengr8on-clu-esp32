/**
 * @file clu_eth.c
 * @brief Ethernet interface: ESP32 internal EMAC + RMII PHY (Grenton GATE: LAN8720A), or a WIZnet
 * W5500 on SPI (KinCony F8, KC868-A8v3).
 *
 * EMAC PHY driver from CLU_ETH_PHY: LAN87xx, or the IDF generic IEEE 802.3 driver for PHYs without
 * their own (e.g. JL1101).
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
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#if CONFIG_CLU_ETH_IF_W5500
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#elif CONFIG_CLU_ETH_PHY_LAN87XX
#include "esp_eth_phy_lan87xx.h"
#endif

static const char *TAG = "clu_eth";

// Above WIFI_STA (100): wired traffic wins the default route.
#define ETH_ROUTE_PRIO 128

#if CONFIG_CLU_ETH_IF_W5500

#define ETH_NAME     "W5500"
#define ETH_SPI_HOST SPI2_HOST

/**
 * @brief Create the W5500 MAC + PHY on SPI2.
 *
 * The W5500 has no MAC address of its own: it gets the chip's Ethernet MAC.
 */
static esp_err_t eth_new(esp_eth_mac_t **mac, esp_eth_phy_t **phy)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_CLU_ETH_SPI_SCLK_GPIO,
        .mosi_io_num = CONFIG_CLU_ETH_SPI_MOSI_GPIO,
        .miso_io_num = CONFIG_CLU_ETH_SPI_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize(ETH_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_CLU_ETH_SPI_INT_GPIO >= 0
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // already installed: fine
        return err;
    }
#endif
    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = CONFIG_CLU_ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = CONFIG_CLU_ETH_SPI_CS_GPIO,
    };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &dev);
    w5500.base.int_gpio_num = CONFIG_CLU_ETH_SPI_INT_GPIO;
    w5500.base.poll_period_ms = CONFIG_CLU_ETH_SPI_INT_GPIO < 0 ? 10 : 0;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 4096;
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = CONFIG_CLU_ETH_SPI_RST_GPIO;

    *mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
    *phy = *mac ? esp_eth_phy_new_w5500(&phy_cfg) : NULL;
    return *phy ? ESP_OK : ESP_ERR_NO_MEM;
}

/** @brief W5500: set the chip's Ethernet MAC address (before esp_eth_start). */
static void eth_set_mac(esp_eth_handle_t eth)
{
    uint8_t m[6];
    if (esp_read_mac(m, ESP_MAC_ETH) == ESP_OK) {
        esp_eth_ioctl(eth, ETH_CMD_S_MAC_ADDR, m);
    }
}

/** @brief Pins for the driver-install error log. */
static void log_install_failed(esp_err_t err)
{
    ESP_LOGE(TAG, "driver install failed: %s (" ETH_NAME " SCLK %d MOSI %d MISO %d CS %d INT %d RST %d)",
             esp_err_to_name(err), CONFIG_CLU_ETH_SPI_SCLK_GPIO, CONFIG_CLU_ETH_SPI_MOSI_GPIO,
             CONFIG_CLU_ETH_SPI_MISO_GPIO, CONFIG_CLU_ETH_SPI_CS_GPIO, CONFIG_CLU_ETH_SPI_INT_GPIO,
             CONFIG_CLU_ETH_SPI_RST_GPIO);
}

#else // CONFIG_CLU_ETH_IF_EMAC

#if CONFIG_CLU_ETH_PHY_LAN87XX
#define ETH_NAME         "EMAC + LAN8720A"
#define ETH_PHY_NEW(cfg) esp_eth_phy_new_lan87xx(cfg)
#else
#define ETH_NAME         "EMAC + generic 802.3 PHY"
#define ETH_PHY_NEW(cfg) esp_eth_phy_new_generic(cfg)
#endif

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

/** @brief Create the internal EMAC + RMII PHY. */
static esp_err_t eth_new(esp_eth_mac_t **mac, esp_eth_phy_t **phy)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = CONFIG_CLU_ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = CONFIG_CLU_ETH_MDIO_GPIO;
    emac_cfg.clock_config.rmii.clock_mode = ETH_CLK_MODE;
    emac_cfg.clock_config.rmii.clock_gpio = ETH_CLK_GPIO;

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = CONFIG_CLU_ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = CONFIG_CLU_ETH_PHY_RST_GPIO;

    *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    *phy = *mac ? ETH_PHY_NEW(&phy_cfg) : NULL;
    return *phy ? ESP_OK : ESP_ERR_NO_MEM;
}

/** @brief EMAC: the MAC address comes from eFuse already. */
static void eth_set_mac(esp_eth_handle_t eth)
{
    (void)eth;
}

/** @brief Pins for the driver-install error log (no PHY answering: wrong address / clock). */
static void log_install_failed(esp_err_t err)
{
    ESP_LOGE(TAG, "driver install failed: %s (" ETH_NAME " addr %d, MDC %d, MDIO %d, clock GPIO%d %s)",
             esp_err_to_name(err), CONFIG_CLU_ETH_PHY_ADDR, CONFIG_CLU_ETH_MDC_GPIO, CONFIG_CLU_ETH_MDIO_GPIO,
             ETH_CLK_GPIO, ETH_CLK_MODE == EMAC_CLK_EXT_IN ? "in" : "out");
}

#endif // CONFIG_CLU_ETH_IF_W5500

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

    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    esp_eth_handle_t eth = NULL;
    esp_err_t err = eth_new(&mac, &phy);
    if (err == ESP_OK) {
        esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
        err = esp_eth_driver_install(&cfg, &eth);
    }
    if (err != ESP_OK) {
        // No chip answering (wrong pins / address / clock) ends up here: run on without Ethernet.
        log_install_failed(err);
        if (phy) {
            phy->del(phy);
        }
        if (mac) {
            mac->del(mac);
        }
        esp_netif_destroy(netif);
        return;
    }
    eth_set_mac(eth);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL));
    clu_net_init(CLU_NET_ETH, netif); // after attach: DHCP / static needs the started netif driver
    uint8_t m[6];
    esp_eth_ioctl(eth, ETH_CMD_G_MAC_ADDR, m);
    ESP_LOGI(TAG, ETH_NAME " up, MAC " MACSTR, MAC2STR(m));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}

#else

void clu_eth_start(void) {}

#endif // CONFIG_CLU_ETH
