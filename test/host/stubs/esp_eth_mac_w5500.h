// Host stub: the WIZnet W5500 MAC driver (clu_eth.c), implemented by the test.
#pragma once
#include "driver/spi_master.h"
#include "esp_eth.h"
typedef struct {
    struct {
        int int_gpio_num;
        uint32_t poll_period_ms;
    } base;
    spi_host_device_t spi_host_id;
    spi_device_interface_config_t *spi_devcfg;
} eth_w5500_config_t;
#define ETH_W5500_DEFAULT_CONFIG(spi_host, spi_devcfg_p) \
    {.base = {.int_gpio_num = 4}, .spi_host_id = (spi_host), .spi_devcfg = (spi_devcfg_p)}
esp_eth_mac_t *esp_eth_mac_new_w5500(const eth_w5500_config_t *w5500_config, const eth_mac_config_t *mac_config);
