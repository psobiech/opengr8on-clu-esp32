/**
 * @file udp_server.h
 * @brief CLU UDP protocol server (port 1234): OM discovery, Lua calls, client reports.
 */
#pragma once

#include <stdint.h>

/** @brief UDP port of the CLU protocol. */
#define CLU_UDP_PORT 1234

/** @brief Start the UDP server task. */
void udp_server_start(void);

/**
 * @brief Set the current STA IPv4 address (called by the WiFi code).
 *
 * @param ip_be IPv4 address in network byte order.
 */
void udp_server_set_local_ip(uint32_t ip_be);

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
