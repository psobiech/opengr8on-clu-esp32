/**
 * @file clu_sec_engine.h
 * @brief Secure CLU channel engine: wire format, sessions, replay window and peer pins.
 *
 * Transport-agnostic and single-threaded (the caller serialises access): datagrams go in through
 * sec_engine_input() and out through sec_ops_t::send; decrypted payloads come out through
 * sec_ops_t::deliver. No FreeRTOS / lwIP / NVS, so the host tests run two engines against each other.
 *
 * Wire format (all integers little-endian):
 *
 *     PROBE      08 | nonce(4) | zero padding to SEC_PROBE_LEN     (no reply amplification)
 *     PROBE_RESP 09 | nonce(4) | version(1) | sn(4) | static pub(32)
 *     HS_INIT    01 | sender_idx(4) | Noise IK msg1 (payload "1:<sn>:<c|h>")
 *     HS_RESP    02 | sender_idx(4) | receiver_idx(4) | Noise IK msg2 (payload "1:<sn>:c")
 *     DATA       04 | receiver_idx(4) | counter(8) | AES-256-GCM(payload) | tag(16)
 *
 * DATA uses the 13-byte header as associated data and the counter as the Noise nonce; a 64-entry
 * sliding window rejects replays.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clu_noise.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_CLU_SEC_MAX_PINS
#define CONFIG_CLU_SEC_MAX_PINS 16
#endif
#ifndef CONFIG_CLU_SEC_MAX_SESSIONS
#define CONFIG_CLU_SEC_MAX_SESSIONS 8
#endif

#define SEC_VERSION 2

#define SEC_T_HS_INIT    1
#define SEC_T_HS_RESP    2
#define SEC_T_DATA       4
#define SEC_T_PROBE      8
#define SEC_T_PROBE_RESP 9

#define SEC_PROBE_LEN      48
#define SEC_PROBE_RESP_LEN (1 + 4 + 1 + 4 + NOISE_KEY_LEN)
#define SEC_DATA_HDR       13
#define SEC_DATA_OVERHEAD  (SEC_DATA_HDR + NOISE_TAG_LEN)

#define SEC_REKEY_AFTER_MS   (3600u * 1000u)          /**< Initiator starts a new handshake. */
#define SEC_REJECT_AFTER_MS  (SEC_REKEY_AFTER_MS + 120u * 1000u) /**< Session no longer usable. */
#define SEC_IDLE_MS          (600u * 1000u)           /**< Unused session dropped. */
#define SEC_REKEY_AFTER_MSGS (1ull << 20)
#define SEC_HS_TIMEOUT_MS    4000u                    /**< Pending initiator handshake given up. */
#define SEC_HS_SKEW_MS       (3600u * 1000u)          /**< Max |HS_INIT ts - own clock| when both have one. */
#define SEC_HS_SEEN          16                       /**< Initiators tracked for HS_INIT replay checks. */
#define SEC_HS_RATE_PER_S    4u                       /**< Responder handshake budget (burst = rate). */

#define SEC_PIN_CLU  1u /**< Pinned peer is a CLU (has an SN). */
#define SEC_PIN_HOST 2u /**< Pinned peer is a host tool. */

/** @brief One pinned peer key. Stored in NVS as a raw array. */
typedef struct {
    uint8_t pub[NOISE_KEY_LEN];
    uint32_t serial; /**< CLU serial, 0 for host tools. */
    uint32_t peer_ip; /**< Last seen IPv4, host order (0 = unknown). */
    uint8_t flags; /**< SEC_PIN_*; 0 = free slot. */
    uint8_t reserved[3];
} sec_pin_t;

/** @brief Pin table. */
typedef struct {
    sec_pin_t entries[CONFIG_CLU_SEC_MAX_PINS];
} sec_pins_t;

/** @brief Result of sec_pins_check(). */
typedef enum {
    SEC_PIN_OK,       /**< Key already pinned (ip / serial refreshed). */
    SEC_PIN_ADDED,    /**< New key pinned (pairing allowed). */
    SEC_PIN_UNKNOWN,  /**< Key not pinned and pairing not allowed. */
    SEC_PIN_CONFLICT, /**< SN already pinned with another key. */
    SEC_PIN_FULL,     /**< No free slot. */
} sec_pin_result_t;

/** @brief Sliding replay window. */
typedef struct {
    uint64_t top;    /**< Highest counter accepted. */
    uint64_t bitmap; /**< Bit i = counter top - i accepted. */
    bool any;
} sec_replay_t;

/** @brief Session slot state. */
typedef enum {
    SEC_S_FREE,
    SEC_S_INIT_SENT, /**< Initiator waiting for HS_RESP. */
    SEC_S_ESTABLISHED,
} sec_state_t;

/** @brief One session (or pending initiator handshake). */
typedef struct {
    sec_state_t state;
    bool initiator;
    uint32_t local_idx;
    uint32_t remote_idx;
    uint32_t peer_ip; /**< Peer, host order. */
    uint16_t port;
    uint32_t peer_sn;
    uint8_t peer_pub[NOISE_KEY_LEN];
    uint8_t send_k[NOISE_KEY_LEN];
    uint8_t recv_k[NOISE_KEY_LEN];
    uint64_t send_n;
    sec_replay_t replay;
    uint32_t created_ms;
    uint32_t last_ms;
    /**
     * Responder: the initiator's key is new and gets pinned on the first authentic DATA (proof that
     * the initiator finished the handshake), so a lost HS_RESP never leaves a one-sided pin.
     */
    bool pin_pending;
    /**
     * Usable for sending: initiator once HS_RESP arrived, responder once the first authentic DATA
     * arrived (the initiator may have completed another, retransmitted copy of the handshake).
     */
    bool confirmed;
    uint8_t peer_flags;
    noise_hs_t handshake; /**< Initiator only, while SEC_S_INIT_SENT. */
    uint8_t hs_msg[5 + NOISE_IK_MSG1_LEN + 24]; /**< Initiator: HS_INIT as sent (retransmission). */
    uint16_t hs_len;
} sec_session_t;

/** @brief Callbacks into the transport / platform. */
typedef struct {
    /** Send one datagram to @p peer_ip (host order) : @p port. */
    void (*send)(void *ctx, uint32_t peer_ip, uint16_t port, const uint8_t *buf, size_t len);
    /** A DATA payload arrived on session @p handle (reply with sec_engine_send(handle, ...)). */
    void (*deliver)(void *ctx, uint32_t handle, const sec_session_t *session, const uint8_t *plain, size_t len);
    /** A pending handshake to @p peer_ip finished (@p succeeded) or failed / timed out. */
    void (*handshake_done)(void *ctx, uint32_t peer_ip, bool succeeded);
    /** True while new peer keys may be pinned (setup window). */
    bool (*pairing_allowed)(void *ctx);
    /** The pin table changed (persist it). */
    void (*pins_changed)(void *ctx);
    /** Random bytes. */
    void (*random)(void *ctx, uint8_t *out, size_t len);
    void *ctx;
    /** Wall clock, unix ms; 0 = not valid / unknown (optional, NULL = none). Placed last: NULL by default. */
    uint64_t (*wall_ms)(void *ctx);
} sec_ops_t;

/** @brief Last accepted HS_INIT timestamp of one initiator key (RAM only). */
typedef struct {
    bool used;
    uint8_t pub[NOISE_KEY_LEN];
    uint64_t timestamp_ms;
    uint32_t at_ms;
} sec_hs_seen_t;

/** @brief Engine state. */
typedef struct {
    noise_keypair_t local_static; /**< Local static key. */
    uint32_t serial;
    sec_pins_t pins;
    sec_session_t sess[CONFIG_CLU_SEC_MAX_SESSIONS];
    sec_ops_t ops;
    uint32_t hs_tokens_ms; /**< Token bucket: time when the bucket was last full - budget. */
    bool hs_bucket_init;
    uint64_t last_ts; /**< Last HS_INIT timestamp sent (kept strictly increasing). */
    sec_hs_seen_t seen[SEC_HS_SEEN];
} sec_engine_t;

// --- replay window --------------------------------------------------------------------------

/** @brief Accept @p counter once; false for a replay or a counter older than the window. */
bool sec_replay_check(sec_replay_t *replay, uint64_t counter);

// --- pins -----------------------------------------------------------------------------------

/** @brief Pin holding @p pub, or NULL. */
sec_pin_t *sec_pins_by_pub(sec_pins_t *pins, const uint8_t pub[NOISE_KEY_LEN]);
/** @brief Pin of CLU @p serial (non-zero), or NULL. */
sec_pin_t *sec_pins_by_sn(sec_pins_t *pins, uint32_t serial);
/** @brief Pin last seen at @p peer_ip (non-zero), or NULL. */
sec_pin_t *sec_pins_by_ip(sec_pins_t *pins, uint32_t peer_ip);

/**
 * @brief Check a peer key against the pins and pin it if allowed.
 *
 * A known key refreshes peer_ip / serial. An unknown key is pinned only with @p allow_new, and never when
 * its non-zero @p serial is already pinned with another key. A new pin takes @p peer_ip over from any
 * other pin that had it.
 *
 * @param changed Set true when the table changed (persist it).
 */
sec_pin_result_t sec_pins_check(sec_pins_t *pins, const uint8_t pub[NOISE_KEY_LEN], uint32_t serial, uint32_t peer_ip,
                                uint8_t flags, bool allow_new, bool *changed);

/**
 * @brief Remove the pin matching @p what: a decimal SN or a hex fingerprint prefix (>= 8 chars).
 * @return Number of pins removed.
 */
int sec_pins_forget(sec_pins_t *pins, const char *what);

/** @brief Key fingerprint: first 8 bytes of SHA-256(pub) as 16 lowercase hex chars + NUL. */
void sec_fingerprint(const uint8_t pub[NOISE_KEY_LEN], char out[17]);

// --- engine ---------------------------------------------------------------------------------

/** @brief Initialise with the local static key, own SN and callbacks; pins start empty. */
void sec_engine_init(sec_engine_t *engine, const noise_keypair_t *local_key, uint32_t serial, const sec_ops_t *ops);

/** @brief Feed one received datagram. */
void sec_engine_input(sec_engine_t *engine, uint32_t peer_ip, uint16_t port, const uint8_t *buf, size_t len, uint32_t now_ms);

/** @brief Build a PROBE datagram with @p nonce into @p out (SEC_PROBE_LEN bytes). */
size_t sec_probe_build(uint32_t nonce, uint8_t out[SEC_PROBE_LEN]);

/**
 * @brief Parse a PROBE_RESP.
 * @return true if it answers @p nonce; fills @p serial and @p pub.
 */
bool sec_probe_parse(const uint8_t *buf, size_t len, uint32_t nonce, uint32_t *serial, uint8_t pub[NOISE_KEY_LEN]);

/**
 * @brief Start a handshake with the pinned-or-candidate key @p peer_pub at @p peer_ip : @p port.
 *
 * The responder's key is not pinned here: the caller pins it once the handshake succeeds
 * (handshake_done ok), so a peer that refuses us is never pinned.
 *
 * @return 0, or -1 (no slot / crypto error).
 */
int sec_engine_connect(sec_engine_t *engine, uint32_t peer_ip, uint16_t port, const uint8_t peer_pub[NOISE_KEY_LEN],
                       uint32_t now_ms);

/**
 * @brief Newest confirmed session to @p peer_ip, or NULL.
 * @param rekey Out (optional): true when the initiator should start a new handshake.
 */
const sec_session_t *sec_engine_session_for(sec_engine_t *engine, uint32_t peer_ip, uint32_t now_ms, bool *rekey);

/**
 * @brief Resend the pending HS_INIT to @p peer_ip unchanged (slow responder / lost packet).
 *
 * Unlike a new sec_engine_connect(), an HS_RESP to the first copy still completes the handshake.
 * @return 0, or -1 when nothing is pending.
 */
int sec_engine_retransmit(sec_engine_t *engine, uint32_t peer_ip);

/** @brief True while a handshake to @p peer_ip is pending. */
bool sec_engine_pending(const sec_engine_t *engine, uint32_t peer_ip);

/**
 * @brief Encrypt and send @p plain on session @p handle (local index).
 * @return 0, or -1 (unknown / expired session, too long).
 */
int sec_engine_send(sec_engine_t *engine, uint32_t handle, const uint8_t *plain, size_t len, uint32_t now_ms);

/** @brief Expire idle / old sessions and time out pending handshakes. */
void sec_engine_tick(sec_engine_t *engine, uint32_t now_ms);

/** @brief Number of established sessions. */
int sec_engine_session_count(const sec_engine_t *engine);

#ifdef __cplusplus
}
#endif
