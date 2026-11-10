/**
 * @file clu_crypto.h
 * @brief AES-128-CBC + PKCS#7 for the CLU UDP protocol (port of src/CipherKey.ts encrypt/decrypt).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief AES-128 key and CBC IV. */
typedef struct {
    uint8_t key[16];   /**< AES-128 key. */
    uint8_t iv[16];    /**< CBC initialisation vector. */
} clu_cipher_t;

/**
 * @brief Initialise the crypto backend (PSA).
 * @return ESP_OK, or ESP_FAIL if PSA init failed.
 */
esp_err_t clu_crypto_init(void);

/**
 * @brief Encrypt with AES-128-CBC and PKCS#7 padding.
 *
 * @param c   Key and IV.
 * @param in  Plaintext.
 * @param len Plaintext length.
 * @param out Ciphertext buffer; must hold len + 16 bytes.
 * @note Plaintext is limited to 1500 bytes (one UDP datagram).
 * @param cap Capacity of @p out.
 * @return Ciphertext length, or -1 on error.
 */
int clu_encrypt(const clu_cipher_t *c, const uint8_t *in, size_t len, uint8_t *out, size_t cap);

/**
 * @brief Decrypt AES-128-CBC and strip PKCS#7 padding.
 *
 * @param c   Key and IV.
 * @param in  Ciphertext (multiple of 16 bytes).
 * @param len Ciphertext length.
 * @param out Plaintext buffer; must hold len bytes.
 * @param cap Capacity of @p out.
 * @return Plaintext length, or -1 (bad length/padding).
 */
int clu_decrypt(const clu_cipher_t *c, const uint8_t *in, size_t len, uint8_t *out, size_t cap);

/**
 * @brief Fill a buffer with random bytes from the hardware RNG (esp_fill_random).
 * @param out Destination.
 * @param len Number of bytes.
 */
void clu_random(uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif
