/**
 * @file udp_server.h
 * @brief CLU UDP protocol server (port 1234): OM discovery, Lua calls, client reports.
 */
#pragma once

#include <stdint.h>

/** @brief UDP port of the CLU protocol. */
#define CLU_UDP_PORT 1234

/**
 * @brief Start the UDP server tasks. Needs lwIP (esp_netif_init()), not an IP address.
 */
void udp_server_start(void);

/**
 * @brief An interface (WiFi / Ethernet) got its first IP: start the setup-window countdown (once
 * per boot).
 *
 * Until then the window stays open, so the time spent waiting for the network does not use it up.
 * Safe from the event loop task.
 */
void udp_server_net_up(void);

/**
 * @brief Send an unsolicited clientReport push.
 *
 * Sends "resp:0.0.0.0:00000000:<body>" encrypted with the project key.
 *
 * @param addr Destination IPv4 address (dotted).
 * @param port Destination UDP port.
 * @param body Report body.
 */
void udp_server_send_report(const char *addr, int port, const char *body);

/**
 * @brief Run a Lua call on another CLU.
 *
 * The call is queued; the reply (or NULL on timeout) is posted with clu_lua_post_remote().
 *
 * @param ip     Remote CLU IPv4 address in host byte order.
 * @param script Lua expression to evaluate remotely.
 * @return Call id, 0 if the call was dropped.
 */
uint32_t udp_server_remote_exec(uint32_t ip, const char *script);
