/**
 * @file clu_crypto.c
 * @brief AES-128-CBC with PKCS#7 padding on top of PSA crypto (mbedtls 4).
 *
 * Padding is done here (clu_keys.c); PSA runs raw CBC without padding.
 */
#include "clu_crypto.h"

#include <stdbool.h>
#include <string.h>

#include "clu_keys.h"
#include "esp_random.h"
#include "psa/crypto.h"

esp_err_t clu_crypto_init(void)
{
    return psa_crypto_init() == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}

void clu_random(uint8_t *out, size_t len)
{
    esp_fill_random(out, len);
}

/**
 * @brief One-shot AES-128-CBC (no padding) with a temporary PSA key.
 *
 * @param c   Key and IV.
 * @param enc true = encrypt, false = decrypt.
 * @param in  Input (multiple of 16 bytes).
 * @param len Input length.
 * @param out Output buffer.
 * @param cap Capacity of @p out.
 * @return Output length, or -1 on PSA error.
 */
static int aes_cbc(const clu_cipher_t *cipher, bool enc, const uint8_t *in, size_t len,
                   uint8_t *out, size_t cap)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, enc ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CBC_NO_PADDING);

    psa_key_id_t id = 0;
    if (psa_import_key(&attr, cipher->key, sizeof cipher->key, &id) != PSA_SUCCESS) {
        return -1;
    }

    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    size_t update_bytes = 0, finish_bytes = 0;
    psa_status_t status = enc ? psa_cipher_encrypt_setup(&operation, id, PSA_ALG_CBC_NO_PADDING)
                          : psa_cipher_decrypt_setup(&operation, id, PSA_ALG_CBC_NO_PADDING);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(&operation, cipher->iv, sizeof cipher->iv);
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(&operation, in, len, out, cap, &update_bytes);
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&operation, out + update_bytes, cap - update_bytes, &finish_bytes);
    }
    if (status != PSA_SUCCESS) {
        psa_cipher_abort(&operation);
    }
    psa_destroy_key(id);
    return status == PSA_SUCCESS ? (int)(update_bytes + finish_bytes) : -1;
}

int clu_encrypt(const clu_cipher_t *cipher, const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    if (cap < len + 16) {
        return -1;
    }
    // Pad in the output buffer, then encrypt in place-safe copy.
    uint8_t tmp[1500 + 16];
    if (len > 1500) {
        return -1;
    }
    memcpy(tmp, in, len);
    size_t plen = clu_pkcs7_pad(tmp, len);
    return aes_cbc(cipher, true, tmp, plen, out, cap);
}

int clu_decrypt(const clu_cipher_t *cipher, const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    if (len == 0 || len % 16 || cap < len) {
        return -1;
    }
    int decrypted_len = aes_cbc(cipher, false, in, len, out, cap);
    if (decrypted_len < 0) {
        return -1;
    }
    return clu_pkcs7_unpad(out, (size_t)decrypted_len);
}
