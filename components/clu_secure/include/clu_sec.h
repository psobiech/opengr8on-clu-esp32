/**
 * @file clu_sec.h
 * @brief Secure CLU channel service (Noise IK over UDP, see SECURE.md).
 *
 * Owns the UDP socket on CONFIG_CLU_SEC_PORT, the device's static X25519 key and the pinned peer
 * keys (NVS namespace "sec"), and serves:
 *  - inbound requests from paired CLUs / host tools (handed to clu_sec_cfg_t::handle_request on a
 *    worker task);
 *  - outbound calls (clu_sec_call), pairing new CLUs automatically while the setup window is open
 *    and reporting peers that do not speak the channel as legacy.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Platform hooks. */
typedef struct {
    /** Device serial number (sent in handshakes and probe answers). */
    uint32_t serial_number;
    /** True while new peer keys may be pinned (setup window). */
    bool (*pairing_allowed)(void);
    /** Wall clock, unix ms; 0 = no valid time (handshake replay protection then is off). Optional. */
    uint64_t (*wall_ms)(void);
    /**
     * Handle a decrypted request from a paired peer.
     *
     * @param script  NUL-terminated script.
     * @param from_ip Peer IPv4, network order.
     * @param out     Result value buffer (no framing).
     * @param cap     Capacity of @p out.
     * @return Result length (0 = empty value).
     */
    size_t (*handle_request)(const char *script, uint32_t from_ip, char *out, size_t cap);
} clu_sec_cfg_t;

/** @brief Result of clu_sec_call(). */
typedef enum {
    CLU_SEC_OK,     /**< Reply received over the secure channel. */
    CLU_SEC_LEGACY, /**< Peer does not speak the channel / cannot be paired now: use the legacy protocol. */
    CLU_SEC_FAIL,   /**< Paired peer unreachable, refused, or no reply: do not fall back. */
} clu_sec_result_t;

/**
 * @brief Load or create the static key, load the pins, open the socket and start the tasks.
 * @return true on success (the channel is then active).
 */
bool clu_sec_start(const clu_sec_cfg_t *cfg);

/** @brief True once clu_sec_start() succeeded. */
bool clu_sec_active(void);

/**
 * @brief Run @p script on the CLU at @p peer_ip and wait for its result value.
 *
 * The call is a binary REQ (call id + script) retransmitted until the peer ACKs it, answered by a
 * RESP (status + value); the peer executes a call at most once. Unknown peers are probed; a peer
 * answering the probe is paired (pinned after a successful handshake) while pairing_allowed().
 * Peers that are not pinned and cannot be paired are reported as CLU_SEC_LEGACY (cached for 10
 * minutes). A pinned peer never yields CLU_SEC_LEGACY. Not reentrant: one caller (remote-call task).
 *
 * @param peer_ip   Peer IPv4, host order.
 * @param script     Lua script / expression to run remotely.
 * @param out        Result value, NUL-terminated.
 * @param cap        Capacity of @p out.
 * @param timeout_ms Wait for the result (the probe / handshake of a first contact come on top).
 */
clu_sec_result_t clu_sec_call(uint32_t peer_ip, const char *script, char *out, size_t cap, uint32_t timeout_ms);

/** @brief True if @p peer_ip (host order) belongs to a pinned peer (no legacy fallback for it). */
bool clu_sec_is_pinned_ip(uint32_t peer_ip);

/** @brief Own static public key fingerprint (16 hex chars + NUL); "" when inactive. */
void clu_sec_fingerprint(char out[17]);

/**
 * @brief Own serial and static public key as `<serial>:<64 hex>` for the legacy pubkey request.
 * @return Length written, 0 when inactive.
 */
size_t clu_sec_pubkey_text(char *out, size_t cap);

/**
 * @brief Forget pins matching @p what (decimal SN or fingerprint prefix >= 8 hex chars).
 * @return Number of pins removed.
 */
int clu_sec_forget(const char *what);

/**
 * @brief Human-readable status: fingerprint, pins (serial, ip, fingerprint) and session count.
 * @return Length written.
 */
size_t clu_sec_info(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
