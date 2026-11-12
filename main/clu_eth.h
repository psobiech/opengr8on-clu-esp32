/**
 * @file clu_eth.h
 * @brief Ethernet interface: ESP32 internal EMAC + LAN8720A RMII PHY (CLU_ETH).
 */
#pragma once

/**
 * @brief Create the Ethernet netif (IP config from clu_net), install the EMAC + LAN8720A driver
 * from menuconfig and start it.
 *
 * Requires esp_netif_init() and the default event loop. A missing / dead PHY is logged and the
 * firmware runs on without Ethernet. No-op when CLU_ETH is off.
 */
void clu_eth_start(void);
