/**
 * @file clu_noise.h
 * @brief Noise_IK_25519_AESGCM_SHA256 handshake and AEAD primitives on the PSA crypto API.
 *
 * Implements the subset of the Noise Protocol Framework (revision 34) needed by the secure CLU
 * channel: the IK pattern with X25519, AES-256-GCM and SHA-256, no PSK. Pure PSA calls, no
 * FreeRTOS / lwIP, so the host tests link it against the system mbedtls.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOISE_KEY_LEN  32 /**< X25519 key / AES-256 key / SHA-256 hash length. */
#define NOISE_TAG_LEN  16 /**< AES-GCM tag length. */
/** @brief IK msg1 length without payload: e + enc(s) + tag of payload. */
#define NOISE_IK_MSG1_LEN (NOISE_KEY_LEN + NOISE_KEY_LEN + NOISE_TAG_LEN + NOISE_TAG_LEN)
/** @brief IK msg2 length without payload: e + tag of payload. */
#define NOISE_IK_MSG2_LEN (NOISE_KEY_LEN + NOISE_TAG_LEN)

/** @brief X25519 key pair (raw private scalar and public u-coordinate). */
typedef struct {
    uint8_t priv[NOISE_KEY_LEN];
    uint8_t pub[NOISE_KEY_LEN];
} noise_keypair_t;

/** @brief Noise CipherState: key and 64-bit nonce. */
typedef struct {
    uint8_t key[NOISE_KEY_LEN];
    uint64_t nonce;
    bool has_key;
} noise_cipher_t;

/** @brief Noise HandshakeState for the IK pattern. */
typedef struct {
    noise_cipher_t cipher;
    uint8_t chaining_key[NOISE_KEY_LEN];
    uint8_t hash[NOISE_KEY_LEN];
    bool initiator;
    bool has_local_ephemeral; /**< Ephemeral already set; otherwise generated on write. */
    noise_keypair_t local_static;
    noise_keypair_t local_ephemeral; /**< Local ephemeral key. */
    uint8_t remote_static[NOISE_KEY_LEN]; /**< Remote static (known to the initiator, learned by the responder). */
    uint8_t remote_ephemeral[NOISE_KEY_LEN];
} noise_hs_t;

/**
 * @brief Derive the public key from a raw private scalar.
 * @return 0, or -1 on PSA error.
 */
int noise_keypair_from_priv(noise_keypair_t *keypair, const uint8_t priv[NOISE_KEY_LEN]);

/**
 * @brief Generate a fresh key pair from the PSA RNG.
 * @return 0, or -1 on PSA error.
 */
int noise_keypair_generate(noise_keypair_t *keypair);

/**
 * @brief Start an IK handshake.
 *
 * @param handshake        State to initialise.
 * @param initiator true for the initiator (must pass @p remote_static), false for the responder.
 * @param prologue       Prologue bound into the handshake hash (both sides must agree).
 * @param prologue_len   Prologue length.
 * @param local_static   Local static key pair.
 * @param remote_static  Responder's static public key (initiator only, NULL for the responder).
 */
void noise_ik_init(noise_hs_t *handshake, bool initiator, const uint8_t *prologue, size_t prologue_len,
                   const noise_keypair_t *local_static, const uint8_t *remote_static);

/**
 * @brief Initiator: write msg1 (-> e, es, s, ss) with @p payload.
 * @return Message length (NOISE_IK_MSG1_LEN + payload_len), or -1.
 */
int noise_ik_write_msg1(noise_hs_t *handshake, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t cap);

/**
 * @brief Responder: read msg1; on success handshake->remote_static holds the initiator's static key.
 * @return Payload length written to @p payload, or -1 (bad length / authentication failure).
 */
int noise_ik_read_msg1(noise_hs_t *handshake, const uint8_t *msg, size_t msg_len, uint8_t *payload, size_t cap);

/**
 * @brief Responder: write msg2 (<- e, ee, se) with @p payload.
 * @return Message length (NOISE_IK_MSG2_LEN + payload_len), or -1.
 */
int noise_ik_write_msg2(noise_hs_t *handshake, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t cap);

/**
 * @brief Initiator: read msg2.
 * @return Payload length, or -1.
 */
int noise_ik_read_msg2(noise_hs_t *handshake, const uint8_t *msg, size_t msg_len, uint8_t *payload, size_t cap);

/**
 * @brief Split the finished handshake into transport keys (Noise Split()).
 *
 * @param handshake        Finished handshake (wiped afterwards).
 * @param send_key  Key for messages this side sends.
 * @param recv_key  Key for messages this side receives.
 */
void noise_split(noise_hs_t *handshake, uint8_t send_key[NOISE_KEY_LEN], uint8_t recv_key[NOISE_KEY_LEN]);

/**
 * @brief AES-256-GCM encrypt with the Noise nonce layout (4 zero bytes, 64-bit big-endian @p counter).
 * @return Ciphertext length (input_len + NOISE_TAG_LEN), or -1.
 */
int noise_aead_encrypt(const uint8_t key[NOISE_KEY_LEN], uint64_t counter, const uint8_t *associated_data, size_t associated_data_len,
                       const uint8_t *input, size_t input_len, uint8_t *out, size_t cap);

/**
 * @brief AES-256-GCM decrypt and verify (see noise_aead_encrypt()).
 * @return Plaintext length, or -1 (bad tag / length).
 */
int noise_aead_decrypt(const uint8_t key[NOISE_KEY_LEN], uint64_t counter, const uint8_t *associated_data, size_t associated_data_len,
                       const uint8_t *input, size_t input_len, uint8_t *out, size_t cap);

/**
 * @brief SHA-256.
 * @return 0, or -1.
 */
int noise_sha256(const uint8_t *input, size_t input_len, uint8_t out[NOISE_KEY_LEN]);

/** @brief Overwrite @p len bytes at @p buf with zeros (not optimised away). */
void noise_wipe(void *buf, size_t len);

#ifdef __cplusplus
}
#endif
