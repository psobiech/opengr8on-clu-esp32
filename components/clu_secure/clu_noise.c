/**
 * @file clu_noise.c
 * @brief Noise_IK_25519_AESGCM_SHA256 on the PSA crypto API (see clu_noise.h).
 *
 * Follows the Noise specification revision 34 section by section: CipherState (5.1),
 * SymmetricState (5.2), HandshakeState for IK (5.3, 7.5) and the AESGCM / SHA256 / 25519 functions
 * (chapter 12). Every PSA key is imported for one operation and destroyed right after.
 */
#include "clu_noise.h"

#include <string.h>

#include "psa/crypto.h"

static const char PROTOCOL_NAME[] = "Noise_IK_25519_AESGCM_SHA256";

static int write_ephemeral(noise_hs_t *handshake, uint8_t *out);
static int encrypt_and_hash(noise_hs_t *handshake, const uint8_t *plaintext, size_t plaintext_len, uint8_t *out, size_t cap);
static int decrypt_and_hash(noise_hs_t *handshake, const uint8_t *ciphertext, size_t ciphertext_len, uint8_t *out, size_t cap);
static int mix_dh(noise_hs_t *handshake, const noise_keypair_t *keypair, const uint8_t pub[NOISE_KEY_LEN]);
static int mix_key(noise_hs_t *handshake, const uint8_t input_key_material[NOISE_KEY_LEN]);
static int mix_hash(noise_hs_t *handshake, const uint8_t *data, size_t data_len);
static int hkdf2(const uint8_t chaining_key[NOISE_KEY_LEN], const uint8_t *input_key_material,
                 size_t input_key_material_len, uint8_t out1[NOISE_KEY_LEN], uint8_t out2[NOISE_KEY_LEN]);
static int hmac(const uint8_t key[NOISE_KEY_LEN], const uint8_t *first, size_t first_len, const uint8_t *second,
                size_t second_len, uint8_t out[NOISE_KEY_LEN]);
static int dh(const noise_keypair_t *keypair, const uint8_t pub[NOISE_KEY_LEN], uint8_t out[NOISE_KEY_LEN]);
static psa_key_id_t import_x25519(const uint8_t priv[NOISE_KEY_LEN]);
static psa_key_id_t import_gcm(const uint8_t key[NOISE_KEY_LEN], psa_key_usage_t usage);
static void gcm_nonce(uint64_t counter, uint8_t nonce[12]);

// ---------------------------------------------------------------------------
// HandshakeState (IK)
// ---------------------------------------------------------------------------

void noise_ik_init(noise_hs_t *handshake, bool initiator, const uint8_t *prologue, size_t prologue_len,
                   const noise_keypair_t *local_static, const uint8_t *remote_static)
{
    memset(handshake, 0, sizeof *handshake);
    memcpy(handshake->hash, PROTOCOL_NAME, sizeof PROTOCOL_NAME - 1); // 28 <= HASHLEN: zero padded
    memcpy(handshake->chaining_key, handshake->hash, sizeof handshake->chaining_key);
    handshake->initiator = initiator;
    handshake->local_static = *local_static;
    mix_hash(handshake, prologue, prologue_len);
    // Pre-message pattern "<- s": the responder's static key.
    if (initiator) {
        memcpy(handshake->remote_static, remote_static, NOISE_KEY_LEN);
        mix_hash(handshake, handshake->remote_static, NOISE_KEY_LEN);
    } else {
        mix_hash(handshake, handshake->local_static.pub, NOISE_KEY_LEN);
    }
}

int noise_ik_write_msg1(noise_hs_t *handshake, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t cap)
{
    if (!handshake->initiator || cap < NOISE_IK_MSG1_LEN + payload_len) {
        return -1;
    }
    uint8_t *cursor = out;
    if (write_ephemeral(handshake, cursor) || mix_dh(handshake, &handshake->local_ephemeral, handshake->remote_static)) { // e, es
        return -1;
    }
    cursor += NOISE_KEY_LEN;
    int written_len = encrypt_and_hash(handshake, handshake->local_static.pub, NOISE_KEY_LEN, cursor, NOISE_KEY_LEN + NOISE_TAG_LEN); // s
    if (written_len < 0 || mix_dh(handshake, &handshake->local_static, handshake->remote_static)) { // ss
        return -1;
    }
    cursor += written_len;
    written_len = encrypt_and_hash(handshake, payload, payload_len, cursor, cap - (size_t)(cursor - out));
    return written_len < 0 ? -1 : (int)(cursor - out) + written_len;
}

int noise_ik_read_msg1(noise_hs_t *handshake, const uint8_t *msg, size_t msg_len, uint8_t *payload, size_t cap)
{
    if (handshake->initiator || msg_len < NOISE_IK_MSG1_LEN) {
        return -1;
    }
    memcpy(handshake->remote_ephemeral, msg, NOISE_KEY_LEN);
    if (mix_hash(handshake, handshake->remote_ephemeral, NOISE_KEY_LEN) ||
        mix_dh(handshake, &handshake->local_static, handshake->remote_ephemeral)) { // e, es
        return -1;
    }
    msg += NOISE_KEY_LEN;
    if (decrypt_and_hash(handshake, msg, NOISE_KEY_LEN + NOISE_TAG_LEN, handshake->remote_static, NOISE_KEY_LEN) != NOISE_KEY_LEN ||
        mix_dh(handshake, &handshake->local_static, handshake->remote_static)) { // s, ss
        return -1;
    }
    msg += NOISE_KEY_LEN + NOISE_TAG_LEN;
    return decrypt_and_hash(handshake, msg, msg_len - (NOISE_KEY_LEN * 2 + NOISE_TAG_LEN), payload, cap);
}

int noise_ik_write_msg2(noise_hs_t *handshake, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t cap)
{
    if (handshake->initiator || cap < NOISE_IK_MSG2_LEN + payload_len) {
        return -1;
    }
    if (write_ephemeral(handshake, out) || mix_dh(handshake, &handshake->local_ephemeral, handshake->remote_ephemeral) || mix_dh(handshake, &handshake->local_ephemeral, handshake->remote_static)) { // e, ee, se
        return -1;
    }
    int written_len = encrypt_and_hash(handshake, payload, payload_len, out + NOISE_KEY_LEN, cap - NOISE_KEY_LEN);
    return written_len < 0 ? -1 : NOISE_KEY_LEN + written_len;
}

int noise_ik_read_msg2(noise_hs_t *handshake, const uint8_t *msg, size_t msg_len, uint8_t *payload, size_t cap)
{
    if (!handshake->initiator || msg_len < NOISE_IK_MSG2_LEN) {
        return -1;
    }
    memcpy(handshake->remote_ephemeral, msg, NOISE_KEY_LEN);
    if (mix_hash(handshake, handshake->remote_ephemeral, NOISE_KEY_LEN) || mix_dh(handshake, &handshake->local_ephemeral, handshake->remote_ephemeral) ||
        mix_dh(handshake, &handshake->local_static, handshake->remote_ephemeral)) { // e, ee, se
        return -1;
    }
    return decrypt_and_hash(handshake, msg + NOISE_KEY_LEN, msg_len - NOISE_KEY_LEN, payload, cap);
}

void noise_split(noise_hs_t *handshake, uint8_t send_key[NOISE_KEY_LEN], uint8_t recv_key[NOISE_KEY_LEN])
{
    uint8_t first_key[NOISE_KEY_LEN], second_key[NOISE_KEY_LEN];
    hkdf2(handshake->chaining_key, NULL, 0, first_key, second_key);
    memcpy(send_key, handshake->initiator ? first_key : second_key, NOISE_KEY_LEN);
    memcpy(recv_key, handshake->initiator ? second_key : first_key, NOISE_KEY_LEN);
    noise_wipe(first_key, sizeof first_key);
    noise_wipe(second_key, sizeof second_key);
    noise_wipe(handshake, sizeof *handshake);
}

// ---------------------------------------------------------------------------
// Keys, hash, AEAD
// ---------------------------------------------------------------------------

int noise_keypair_generate(noise_keypair_t *keypair)
{
    uint8_t priv[NOISE_KEY_LEN];
    if (psa_generate_random(priv, sizeof priv) != PSA_SUCCESS) {
        return -1;
    }
    int result = noise_keypair_from_priv(keypair, priv);
    noise_wipe(priv, sizeof priv);
    return result;
}

int noise_keypair_from_priv(noise_keypair_t *keypair, const uint8_t priv[NOISE_KEY_LEN])
{
    psa_key_id_t key_id = import_x25519(priv);
    if (!key_id) {
        return -1;
    }
    size_t exported_len = 0;
    psa_status_t status = psa_export_public_key(key_id, keypair->pub, sizeof keypair->pub, &exported_len);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS || exported_len != NOISE_KEY_LEN) {
        return -1;
    }
    if (keypair->priv != priv) {
        memcpy(keypair->priv, priv, NOISE_KEY_LEN);
    }
    return 0;
}

int noise_sha256(const uint8_t *input, size_t input_len, uint8_t out[NOISE_KEY_LEN])
{
    size_t hash_len = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, input, input_len, out, NOISE_KEY_LEN, &hash_len) == PSA_SUCCESS ? 0 : -1;
}

int noise_aead_encrypt(const uint8_t key[NOISE_KEY_LEN], uint64_t counter, const uint8_t *associated_data, size_t associated_data_len,
                       const uint8_t *input, size_t input_len, uint8_t *out, size_t cap)
{
    if (cap < input_len + NOISE_TAG_LEN) {
        return -1;
    }
    psa_key_id_t key_id = import_gcm(key, PSA_KEY_USAGE_ENCRYPT);
    if (!key_id) {
        return -1;
    }
    uint8_t nonce[12];
    gcm_nonce(counter, nonce);
    size_t out_len = 0;
    psa_status_t status =
        psa_aead_encrypt(key_id, PSA_ALG_GCM, nonce, sizeof nonce, associated_data, associated_data_len, input, input_len, out, cap, &out_len);
    psa_destroy_key(key_id);
    return status == PSA_SUCCESS ? (int)out_len : -1;
}

int noise_aead_decrypt(const uint8_t key[NOISE_KEY_LEN], uint64_t counter, const uint8_t *associated_data, size_t associated_data_len,
                       const uint8_t *input, size_t input_len, uint8_t *out, size_t cap)
{
    if (input_len < NOISE_TAG_LEN || cap < input_len - NOISE_TAG_LEN) {
        return -1;
    }
    psa_key_id_t key_id = import_gcm(key, PSA_KEY_USAGE_DECRYPT);
    if (!key_id) {
        return -1;
    }
    uint8_t nonce[12];
    gcm_nonce(counter, nonce);
    size_t out_len = 0;
    psa_status_t status =
        psa_aead_decrypt(key_id, PSA_ALG_GCM, nonce, sizeof nonce, associated_data, associated_data_len, input, input_len, out, cap, &out_len);
    psa_destroy_key(key_id);
    return status == PSA_SUCCESS ? (int)out_len : -1;
}

void noise_wipe(void *buf, size_t len)
{
    volatile uint8_t *cursor = buf;
    while (len--) {
        *cursor++ = 0;
    }
}

// ---------------------------------------------------------------------------
// SymmetricState
// ---------------------------------------------------------------------------

/** @brief Token "e" when writing: generate (unless preset) and send the ephemeral key. */
static int write_ephemeral(noise_hs_t *handshake, uint8_t *out)
{
    if (!handshake->has_local_ephemeral && noise_keypair_generate(&handshake->local_ephemeral)) {
        return -1;
    }
    handshake->has_local_ephemeral = true;
    memcpy(out, handshake->local_ephemeral.pub, NOISE_KEY_LEN);
    return mix_hash(handshake, handshake->local_ephemeral.pub, NOISE_KEY_LEN);
}

static int encrypt_and_hash(noise_hs_t *handshake, const uint8_t *plaintext, size_t plaintext_len, uint8_t *out, size_t cap)
{
    int ciphertext_len;
    if (handshake->cipher.has_key) {
        ciphertext_len = noise_aead_encrypt(handshake->cipher.key, handshake->cipher.nonce, handshake->hash, sizeof handshake->hash, plaintext,
                                            plaintext_len, out, cap);
        if (ciphertext_len < 0) {
            return -1;
        }
        handshake->cipher.nonce++;
    } else {
        if (cap < plaintext_len) {
            return -1;
        }
        memmove(out, plaintext, plaintext_len);
        ciphertext_len = (int)plaintext_len;
    }
    return mix_hash(handshake, out, (size_t)ciphertext_len) ? -1 : ciphertext_len;
}

static int decrypt_and_hash(noise_hs_t *handshake, const uint8_t *ciphertext, size_t ciphertext_len, uint8_t *out, size_t cap)
{
    int plaintext_len;
    if (handshake->cipher.has_key) {
        plaintext_len = noise_aead_decrypt(handshake->cipher.key, handshake->cipher.nonce, handshake->hash, sizeof handshake->hash, ciphertext,
                                           ciphertext_len, out, cap);
        if (plaintext_len < 0) {
            return -1;
        }
        handshake->cipher.nonce++;
    } else {
        if (cap < ciphertext_len) {
            return -1;
        }
        memmove(out, ciphertext, ciphertext_len);
        plaintext_len = (int)ciphertext_len;
    }
    // MixHash over the ciphertext; it may alias out only when there is no key (same bytes then).
    return mix_hash(handshake, ciphertext, ciphertext_len) ? -1 : plaintext_len;
}

/** @brief DH then MixKey(). */
static int mix_dh(noise_hs_t *handshake, const noise_keypair_t *keypair, const uint8_t pub[NOISE_KEY_LEN])
{
    uint8_t shared[NOISE_KEY_LEN];
    int result = dh(keypair, pub, shared);
    if (!result) {
        result = mix_key(handshake, shared);
    }
    noise_wipe(shared, sizeof shared);
    return result;
}

static int mix_key(noise_hs_t *handshake, const uint8_t input_key_material[NOISE_KEY_LEN])
{
    if (hkdf2(handshake->chaining_key, input_key_material, NOISE_KEY_LEN, handshake->chaining_key, handshake->cipher.key)) {
        return -1;
    }
    handshake->cipher.nonce = 0;
    handshake->cipher.has_key = true;
    return 0;
}

static int mix_hash(noise_hs_t *handshake, const uint8_t *data, size_t data_len)
{
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    size_t hash_len = 0;
    psa_status_t status = psa_hash_setup(&operation, PSA_ALG_SHA_256);
    if (status == PSA_SUCCESS) {
        status = psa_hash_update(&operation, handshake->hash, sizeof handshake->hash);
    }
    if (status == PSA_SUCCESS && data_len) {
        status = psa_hash_update(&operation, data, data_len);
    }
    if (status == PSA_SUCCESS) {
        status = psa_hash_finish(&operation, handshake->hash, sizeof handshake->hash, &hash_len);
    }
    if (status != PSA_SUCCESS) {
        psa_hash_abort(&operation);
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

/** @brief HKDF(chaining_key, input_key_material) with two outputs (Noise section 4.3). */
static int hkdf2(const uint8_t chaining_key[NOISE_KEY_LEN], const uint8_t *input_key_material,
                 size_t input_key_material_len, uint8_t out1[NOISE_KEY_LEN], uint8_t out2[NOISE_KEY_LEN])
{
    uint8_t temp_key[NOISE_KEY_LEN], first_output[NOISE_KEY_LEN];
    static const uint8_t one = 1, two = 2;
    int result = hmac(chaining_key, input_key_material, input_key_material_len, NULL, 0, temp_key);
    if (!result) {
        result = hmac(temp_key, &one, 1, NULL, 0, first_output);
    }
    if (!result) {
        result = hmac(temp_key, first_output, sizeof first_output, &two, 1, out2);
    }
    if (!result) {
        memcpy(out1, first_output, sizeof first_output);
    }
    noise_wipe(temp_key, sizeof temp_key);
    noise_wipe(first_output, sizeof first_output);
    return result;
}

/** @brief HMAC-SHA256(key, first || second). */
static int hmac(const uint8_t key[NOISE_KEY_LEN], const uint8_t *first, size_t first_len, const uint8_t *second,
                size_t second_len, uint8_t out[NOISE_KEY_LEN])
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, NOISE_KEY_LEN * 8);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t key_id = 0;
    if (psa_import_key(&attributes, key, NOISE_KEY_LEN, &key_id) != PSA_SUCCESS) {
        return -1;
    }
    psa_mac_operation_t operation = PSA_MAC_OPERATION_INIT;
    size_t mac_len = 0;
    psa_status_t status = psa_mac_sign_setup(&operation, key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS && first_len) {
        status = psa_mac_update(&operation, first, first_len);
    }
    if (status == PSA_SUCCESS && second_len) {
        status = psa_mac_update(&operation, second, second_len);
    }
    if (status == PSA_SUCCESS) {
        status = psa_mac_sign_finish(&operation, out, NOISE_KEY_LEN, &mac_len);
    }
    if (status != PSA_SUCCESS) {
        psa_mac_abort(&operation);
    }
    psa_destroy_key(key_id);
    return status == PSA_SUCCESS && mac_len == NOISE_KEY_LEN ? 0 : -1;
}

/** @brief DH(): X25519; an all-zero result (low-order peer point) is rejected. */
static int dh(const noise_keypair_t *keypair, const uint8_t pub[NOISE_KEY_LEN], uint8_t out[NOISE_KEY_LEN])
{
    psa_key_id_t key_id = import_x25519(keypair->priv);
    if (!key_id) {
        return -1;
    }
    size_t shared_len = 0;
    psa_status_t status =
        psa_raw_key_agreement(PSA_ALG_ECDH, key_id, pub, NOISE_KEY_LEN, out, NOISE_KEY_LEN, &shared_len);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS || shared_len != NOISE_KEY_LEN) {
        return -1;
    }
    uint8_t accumulated = 0;
    for (int i = 0; i < NOISE_KEY_LEN; i++) {
        accumulated |= out[i];
    }
    return accumulated ? 0 : -1;
}

/** @brief Import a raw X25519 private scalar for ECDH (and public key export). */
static psa_key_id_t import_x25519(const uint8_t priv[NOISE_KEY_LEN])
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attributes, 255);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
    psa_key_id_t key_id = 0;
    if (psa_import_key(&attributes, priv, NOISE_KEY_LEN, &key_id) != PSA_SUCCESS) {
        return 0;
    }
    return key_id;
}

/** @brief Import an AES-256-GCM key. */
static psa_key_id_t import_gcm(const uint8_t key[NOISE_KEY_LEN], psa_key_usage_t usage)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_key_id_t key_id = 0;
    return psa_import_key(&attributes, key, NOISE_KEY_LEN, &key_id) == PSA_SUCCESS ? key_id : 0;
}

/** @brief Noise AESGCM nonce: 32 bits of zeros then the big-endian 64-bit counter. */
static void gcm_nonce(uint64_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = (uint8_t)(counter >> (56 - 8 * i));
    }
}
