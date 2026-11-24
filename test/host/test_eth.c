// Host tests for the Ethernet interface (main/clu_eth.c, included: state is static): driver set-up
// and teardown on failure, the netif route priority, MAC address, link events. Built twice: the
// internal EMAC with a LAN8720A (default) and the W5500 on SPI (CONFIG_CLU_ETH_IF_W5500=1).
#include <stdio.h>
#include <string.h>

#define CONFIG_CLU_ETH 1
#if CONFIG_CLU_ETH_IF_W5500
#define CONFIG_CLU_ETH_SPI_SCLK_GPIO 42
#define CONFIG_CLU_ETH_SPI_MOSI_GPIO 43
#define CONFIG_CLU_ETH_SPI_MISO_GPIO 44
#define CONFIG_CLU_ETH_SPI_CS_GPIO   41
#define CONFIG_CLU_ETH_SPI_INT_GPIO  2
#define CONFIG_CLU_ETH_SPI_RST_GPIO  1
#define CONFIG_CLU_ETH_SPI_CLOCK_MHZ 20
#else
#define CONFIG_CLU_ETH_PHY_LAN87XX    1
#define CONFIG_CLU_ETH_CLK_EXT_IN_GPIO0 1
#define CONFIG_CLU_ETH_PHY_ADDR       1
#define CONFIG_CLU_ETH_MDC_GPIO       23
#define CONFIG_CLU_ETH_MDIO_GPIO      18
#define CONFIG_CLU_ETH_PHY_RST_GPIO   5
#endif

#include "../../main/clu_eth.c"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- fakes: state
typedef enum {
    FAIL_NONE,
    FAIL_SPI_BUS,
    FAIL_ISR_SERVICE,
    ISR_SERVICE_ALREADY_INSTALLED,
    FAIL_MAC,
    FAIL_PHY,
    FAIL_DRIVER_INSTALL,
    FAIL_READ_MAC,
} fail_step_t;

const char *const ETH_EVENT = "ETH_EVENT";
static const uint8_t CHIP_ETH_MAC[6] = {0x24, 0x0a, 0xc4, 0x01, 0x02, 0x03};

static fail_step_t s_fail_step;
static struct esp_netif_obj { int unused; } s_fake_netif;
static int s_netifs_created, s_netifs_destroyed;
static esp_netif_inherent_config_t s_last_netif_base;
static const void *s_last_netif_stack;
static esp_eth_mac_t s_fake_mac;
static esp_eth_phy_t s_fake_phy;
static int s_mac_deletes, s_phy_deletes, s_macs_created, s_phys_created;
static eth_mac_config_t s_last_mac_config;
static eth_phy_config_t s_last_phy_config;
static int s_driver_installs;
static int s_driver_handle_target;
static esp_eth_config_t s_last_eth_config;
static int s_netif_attaches, s_starts;
static esp_netif_t *s_initialised_netif;
static clu_net_if_t s_initialised_interface = CLU_NET_IF_COUNT;
static esp_event_handler_t s_eth_handler;
static esp_event_base_t s_eth_handler_base;
static int32_t s_eth_handler_id;
static uint8_t s_mac_set[6];
static int s_mac_sets;
static eth_speed_t s_link_speed;
static eth_duplex_t s_link_duplex;
static int s_link_queries;
#if CONFIG_CLU_ETH_IF_W5500
static spi_bus_config_t s_last_spi_bus;
static spi_host_device_t s_last_spi_host;
static int s_spi_bus_inits;
static eth_w5500_config_t s_last_w5500_config;
static spi_device_interface_config_t s_last_spi_device;
#else
static eth_esp32_emac_config_t s_last_emac_config;
#endif

static void test_failures(void);
static void test_start(void);
static void test_link_events(void);
static void reset_fakes(void);
static esp_err_t delete_mac(esp_eth_mac_t *mac);
static esp_err_t delete_phy(esp_eth_phy_t *phy);
static esp_eth_mac_t *new_mac(const eth_mac_config_t *config);
static esp_eth_phy_t *new_phy(const eth_phy_config_t *config);

int main(void)
{
    s_fake_mac.del = delete_mac;
    s_fake_phy.del = delete_phy;
    test_failures();
    test_start();
    test_link_events();
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- failed set-up
static void test_failures(void)
{
#if CONFIG_CLU_ETH_IF_W5500
    reset_fakes();
    s_fail_step = FAIL_SPI_BUS;
    clu_eth_start();
    check("SPI bus fails: no MAC created, netif destroyed", s_macs_created == 0 && s_driver_installs == 0 &&
                                                                s_netifs_destroyed == 1);

    reset_fakes();
    s_fail_step = FAIL_ISR_SERVICE;
    clu_eth_start();
    check("GPIO ISR service fails: no MAC created, netif destroyed", s_macs_created == 0 && s_netifs_destroyed == 1);
#endif

    reset_fakes();
    s_fail_step = FAIL_MAC;
    clu_eth_start();
    check("MAC fails: no PHY created, nothing to delete", s_phys_created == 0 && s_mac_deletes == 0 &&
                                                              s_phy_deletes == 0 && s_driver_installs == 0);
    check("MAC fails: netif destroyed, not started", s_netifs_destroyed == 1 && s_starts == 0);

    reset_fakes();
    s_fail_step = FAIL_PHY;
    clu_eth_start();
    check("PHY fails: MAC deleted, no driver", s_mac_deletes == 1 && s_phy_deletes == 0 && s_driver_installs == 0 &&
                                                   s_netifs_destroyed == 1);

    reset_fakes();
    s_fail_step = FAIL_DRIVER_INSTALL;
    clu_eth_start();
    check("driver install fails (no chip answering): MAC and PHY deleted",
          s_driver_installs == 1 && s_mac_deletes == 1 && s_phy_deletes == 1 && s_netifs_destroyed == 1);
    check("driver install fails: nothing attached or started", s_netif_attaches == 0 && s_eth_handler == NULL &&
                                                                   s_initialised_netif == NULL && s_starts == 0);
}

// ---------------------------------------------------------------- set-up
static void test_start(void)
{
    reset_fakes();
    clu_eth_start();
    check("netif with Ethernet stack and a route priority above WiFi", s_netifs_created == 1 &&
                                                                          s_last_netif_base.route_prio == 128 &&
                                                                          s_last_netif_stack == ESP_NETIF_NETSTACK_DEFAULT_ETH);
    check("driver installed with the created MAC and PHY", s_last_eth_config.mac == &s_fake_mac &&
                                                               s_last_eth_config.phy == &s_fake_phy);
    check("netif attached, link events handled", s_netif_attaches == 1 && s_eth_handler == on_eth &&
                                                     s_eth_handler_base == ETH_EVENT &&
                                                     s_eth_handler_id == ESP_EVENT_ANY_ID);
    check("clu_net takes over the Ethernet netif", s_initialised_interface == CLU_NET_ETH &&
                                                       s_initialised_netif == &s_fake_netif);
    check("driver started, nothing deleted", s_starts == 1 && s_mac_deletes == 0 && s_phy_deletes == 0 &&
                                                 s_netifs_destroyed == 0);
#if CONFIG_CLU_ETH_IF_W5500
    check("SPI2 bus on the configured pins", s_last_spi_host == SPI2_HOST && s_last_spi_bus.sclk_io_num == 42 &&
                                                 s_last_spi_bus.mosi_io_num == 43 && s_last_spi_bus.miso_io_num == 44 &&
                                                 s_last_spi_bus.quadwp_io_num == -1 && s_last_spi_bus.quadhd_io_num == -1);
    check("W5500 device: mode 0, clock, CS", s_last_spi_device.mode == 0 &&
                                                 s_last_spi_device.clock_speed_hz == 20 * 1000 * 1000 &&
                                                 s_last_spi_device.spics_io_num == 41 && s_last_spi_device.queue_size == 20);
    check("W5500 interrupt pin, no polling", s_last_w5500_config.base.int_gpio_num == 2 &&
                                                 s_last_w5500_config.base.poll_period_ms == 0 &&
                                                 s_last_w5500_config.spi_host_id == SPI2_HOST);
    check("PHY reset pin, bigger RX task stack", s_last_phy_config.reset_gpio_num == 1 &&
                                                     s_last_mac_config.rx_task_stack_size == 4096);
    check("chip's Ethernet MAC set on the W5500", s_mac_sets == 1 && memcmp(s_mac_set, CHIP_ETH_MAC, 6) == 0);

    reset_fakes();
    s_fail_step = ISR_SERVICE_ALREADY_INSTALLED;
    clu_eth_start();
    check("GPIO ISR service already installed: started", s_starts == 1);

    reset_fakes();
    s_fail_step = FAIL_READ_MAC;
    clu_eth_start();
    check("no chip MAC: W5500 keeps its own, started", s_mac_sets == 0 && s_starts == 1);
#else
    check("EMAC: SMI pins", s_last_emac_config.smi_gpio.mdc_num == 23 && s_last_emac_config.smi_gpio.mdio_num == 18);
    check("EMAC: external RMII clock in on GPIO0", s_last_emac_config.clock_config.rmii.clock_mode == EMAC_CLK_EXT_IN &&
                                                       s_last_emac_config.clock_config.rmii.clock_gpio == 0);
    check("PHY address and reset pin", s_last_phy_config.phy_addr == 1 && s_last_phy_config.reset_gpio_num == 5);
    check("EMAC: MAC address left to the eFuse", s_mac_sets == 0);
#endif
}

// ---------------------------------------------------------------- link events
static void test_link_events(void)
{
    esp_eth_handle_t eth_handle = &s_driver_handle_target;
    s_mac_sets = s_starts = 0; // the handler registered by the last start stays
    s_link_speed = ETH_SPEED_100M;
    s_link_duplex = ETH_DUPLEX_FULL;
    s_eth_handler(NULL, ETH_EVENT, ETHERNET_EVENT_CONNECTED, &eth_handle);
    s_link_speed = ETH_SPEED_10M;
    s_link_duplex = ETH_DUPLEX_HALF;
    s_eth_handler(NULL, ETH_EVENT, ETHERNET_EVENT_CONNECTED, &eth_handle);
    s_eth_handler(NULL, ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, NULL);
    s_eth_handler(NULL, ETH_EVENT, ETHERNET_EVENT_START, NULL);
    check("link up: speed and duplex asked for the log, twice each", s_link_queries == 4);
    check("link events change nothing else", s_mac_sets == 0 && s_starts == 0);
}

static void reset_fakes(void)
{
    s_fail_step = FAIL_NONE;
    s_netifs_created = s_netifs_destroyed = 0;
    s_macs_created = s_phys_created = s_mac_deletes = s_phy_deletes = 0;
    s_driver_installs = s_netif_attaches = s_starts = s_mac_sets = 0;
    s_initialised_netif = NULL;
    s_initialised_interface = CLU_NET_IF_COUNT;
    s_eth_handler = NULL;
#if CONFIG_CLU_ETH_IF_W5500
    s_spi_bus_inits = 0;
#endif
}

static esp_err_t delete_mac(esp_eth_mac_t *mac)
{
    s_mac_deletes++;
    return ESP_OK;
}

static esp_err_t delete_phy(esp_eth_phy_t *phy)
{
    s_phy_deletes++;
    return ESP_OK;
}

// ---------------------------------------------------------------- fakes: netif, events, MAC
esp_netif_t *esp_netif_new(const esp_netif_config_t *config)
{
    s_netifs_created++;
    s_last_netif_base = *config->base;
    s_last_netif_stack = config->stack;
    return &s_fake_netif;
}

void esp_netif_destroy(esp_netif_t *netif)
{
    s_netifs_destroyed++;
}

esp_err_t esp_netif_attach(esp_netif_t *netif, void *driver_handle)
{
    s_netif_attaches++;
    return ESP_OK;
}

esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t fn, void *arg)
{
    s_eth_handler_base = base;
    s_eth_handler_id = id;
    s_eth_handler = fn;
    return ESP_OK;
}

void clu_net_init(clu_net_if_t interface_index, esp_netif_t *netif)
{
    s_initialised_interface = interface_index;
    s_initialised_netif = netif;
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    if (s_fail_step == FAIL_READ_MAC || type != ESP_MAC_ETH) {
        return ESP_FAIL;
    }
    memcpy(mac, CHIP_ETH_MAC, 6);
    return ESP_OK;
}

// ---------------------------------------------------------------- fakes: Ethernet driver
esp_err_t esp_eth_driver_install(const esp_eth_config_t *config, esp_eth_handle_t *out_hdl)
{
    s_driver_installs++;
    s_last_eth_config = *config;
    if (s_fail_step == FAIL_DRIVER_INSTALL) {
        return ESP_ERR_TIMEOUT;
    }
    *out_hdl = &s_driver_handle_target;
    return ESP_OK;
}

esp_err_t esp_eth_ioctl(esp_eth_handle_t hdl, esp_eth_io_cmd_t cmd, void *data)
{
    switch (cmd) {
    case ETH_CMD_G_MAC_ADDR: memcpy(data, CHIP_ETH_MAC, 6); break;
    case ETH_CMD_S_MAC_ADDR:
        memcpy(s_mac_set, data, 6);
        s_mac_sets++;
        break;
    case ETH_CMD_G_SPEED:
        *(eth_speed_t *)data = s_link_speed;
        s_link_queries++;
        break;
    case ETH_CMD_G_DUPLEX_MODE:
        *(eth_duplex_t *)data = s_link_duplex;
        s_link_queries++;
        break;
    }
    return ESP_OK;
}

esp_eth_netif_glue_handle_t esp_eth_new_netif_glue(esp_eth_handle_t eth_hdl)
{
    return eth_hdl;
}

esp_err_t esp_eth_start(esp_eth_handle_t hdl)
{
    s_starts++;
    return ESP_OK;
}

#if CONFIG_CLU_ETH_IF_W5500
esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t *bus_config, int dma_chan)
{
    s_spi_bus_inits++;
    s_last_spi_host = host_id;
    s_last_spi_bus = *bus_config;
    return s_fail_step == FAIL_SPI_BUS ? ESP_FAIL : ESP_OK;
}

esp_err_t gpio_install_isr_service(int intr_alloc_flags)
{
    if (s_fail_step == FAIL_ISR_SERVICE) {
        return ESP_ERR_NO_MEM;
    }
    return s_fail_step == ISR_SERVICE_ALREADY_INSTALLED ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_eth_mac_t *esp_eth_mac_new_w5500(const eth_w5500_config_t *w5500_config, const eth_mac_config_t *mac_config)
{
    s_last_w5500_config = *w5500_config;
    s_last_spi_device = *w5500_config->spi_devcfg;
    return new_mac(mac_config);
}

esp_eth_phy_t *esp_eth_phy_new_w5500(const eth_phy_config_t *config)
{
    return new_phy(config);
}
#else
esp_eth_mac_t *esp_eth_mac_new_esp32(const eth_esp32_emac_config_t *esp32_config, const eth_mac_config_t *config)
{
    s_last_emac_config = *esp32_config;
    return new_mac(config);
}

esp_eth_phy_t *esp_eth_phy_new_lan87xx(const eth_phy_config_t *config)
{
    return new_phy(config);
}
#endif

// ---------------------------------------------------------------- fakes: MAC / PHY objects
static esp_eth_phy_t *new_phy(const eth_phy_config_t *config)
{
    s_last_phy_config = *config;
    if (s_fail_step == FAIL_PHY) {
        return NULL;
    }
    s_phys_created++;
    return &s_fake_phy;
}

static esp_eth_mac_t *new_mac(const eth_mac_config_t *config)
{
    s_last_mac_config = *config;
    if (s_fail_step == FAIL_MAC) {
        return NULL;
    }
    s_macs_created++;
    return &s_fake_mac;
}
