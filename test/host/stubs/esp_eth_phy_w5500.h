// Host stub: the WIZnet W5500 PHY driver (clu_eth.c), implemented by the test.
#pragma once
#include "esp_eth.h"
esp_eth_phy_t *esp_eth_phy_new_w5500(const eth_phy_config_t *config);
