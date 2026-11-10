/**
 * @file clu_keys.h
 * @brief CLU UDP key helpers.
 *
 * Port of src/CipherKey.ts (non-AES parts) and src/commands/DiscoverCLUsCommand.ts hash().
 * Pure C, no ESP-IDF deps.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLU_KEY_BYTES        16   /**< AES key length. */
#define CLU_IV_BYTES         16   /**< AES IV length. */
#define CLU_PIN_BYTES        8    /**< Private key (PIN) length. */
#define CLU_RANDOM_BYTES     30   /**< Discovery challenge length. */
#define CLU_RANDOM_ENC_BYTES 32   /**< Encrypted discovery challenge length. */

/** @brief Grenton master key used for the discovery broadcast. */
extern const uint8_t CLU_GRENTON_KEY[16];
/** @brief Default IV of a factory-fresh CLU. */
extern const uint8_t CLU_DEFAULT_IV[16];
/** @brief IV used for the discovery broadcast. */
extern const uint8_t CLU_DEFAULT_BROADCAST_IV[16];

/**
 * @brief Derive a key from an IV and the private key.
 *
 * key[i] = iv[i] ^ pk[i] for i<8, iv[i] ^ pk[15-i] for i>=8.
 *
 * @param iv          16-byte IV.
 * @param private_key 8-byte private key.
 * @param out         16-byte derived key.
 */
void clu_derive_key(const uint8_t iv[16], const uint8_t private_key[8], uint8_t out[16]);

/**
 * @brief Per-device default key from the private key (PIN printed on the sticker).
 * @param private_key 8-byte private key.
 * @param out         16-byte default key.
 */
void clu_default_key(const uint8_t private_key[8], uint8_t out[16]);

/**
 * @brief Discovery proof-of-key hash.
 * @param in  Input bytes.
 * @param len Length of input and output.
 * @param out Output (len bytes).
 */
void clu_discovery_hash(const uint8_t *in, size_t len, uint8_t *out);

/**
 * @brief PKCS#7 pad in place.
 * @param buf Buffer; must have room for len + 16.
 * @param len Data length.
 * @return Padded length.
 */
size_t clu_pkcs7_pad(uint8_t *buf, size_t len);

/**
 * @brief Validate PKCS#7 padding.
 * @param buf Padded data.
 * @param len Padded length.
 * @return Unpadded length, or -1 if padding invalid.
 */
int clu_pkcs7_unpad(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
