/**
 * @file clu_http.h
 * @brief GATE HTTP objects: HttpRequest (class 121) and HttpListener (class 120), http.lua.
 *
 * C side: a worker task runs the outgoing requests (esp_http_client, HTTPS with the built-in
 * certificate bundle) one after another; a server task accepts incoming requests on one TCP port
 * and hands them to the Lua task, which routes them by path to the HttpListener objects and
 * answers with SendResponse. Results and requests reach Lua through one queue, polled by http.lua
 * at every SYSTEM.Loop.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Configuration. */
typedef struct {
    uint16_t port;           /**< HttpListener TCP port (80 on a real GATE). */
    size_t max_body;         /**< Largest request / response body accepted, bytes. */
    int listener_timeout_ms; /**< How long an incoming request waits for SendResponse. */
} clu_http_cfg_t;

/**
 * @brief Register the Lua side (http.lua). Call before clu_lua_start().
 * @param cfg Configuration (copied).
 */
void clu_http_init(const clu_http_cfg_t *cfg);

/** @brief Start the request worker and the listener. Needs lwIP (esp_netif_init()), not an IP. */
void clu_http_start(void);

#ifdef __cplusplus
}
#endif
