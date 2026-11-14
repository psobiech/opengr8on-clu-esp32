/**
 * @file clu_keys.c
 * @brief CLU key derivation, discovery hash and PKCS#7 padding (pure C).
 */
#include "clu_keys.h"

// Factory constants, see src/CipherKey.ts
// DEFAULT_KEY          = hd5SHpxl0N5+WEXTXlPQmw==
// DEFAULT_IV           = ua/jh/kZo9Og15rejhGhFg==
// DEFAULT_BROADCAST_IV = BwYFBAMCAQAEAgkDBAEFBw==
const uint8_t CLU_GRENTON_KEY[16] = {
    0x85, 0xde, 0x52, 0x1e, 0x9c, 0x65, 0xd0, 0xde,
    0x7e, 0x58, 0x45, 0xd3, 0x5e, 0x53, 0xd0, 0x9b,
};
const uint8_t CLU_DEFAULT_IV[16] = {
    0xb9, 0xaf, 0xe3, 0x87, 0xf9, 0x19, 0xa3, 0xd3,
    0xa0, 0xd7, 0x9a, 0xde, 0x8e, 0x11, 0xa1, 0x16,
};
const uint8_t CLU_DEFAULT_BROADCAST_IV[16] = {
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0x04, 0x02, 0x09, 0x03, 0x04, 0x01, 0x05, 0x07,
};

void clu_derive_key(const uint8_t initialization_vector[16], const uint8_t private_key[8], uint8_t out[16])
{
    for (int i = 0; i < 8; i++) {
        out[i] = initialization_vector[i] ^ private_key[i];
    }
    for (int i = 8; i < 16; i++) {
        out[i] = initialization_vector[i] ^ private_key[15 - i];
    }
}

void clu_default_key(const uint8_t private_key[8], uint8_t out[16])
{
    clu_derive_key(CLU_DEFAULT_IV, private_key, out);
}

void clu_discovery_hash(const uint8_t *in, size_t len, uint8_t *out)
{
    if (len == 0) {
        return;
    }
    // OM works on Java signed bytes: -5 % 13 == -5. C's % truncates toward zero
    // like Java's, so do the same on int8_t; unsigned math differs for any byte >= 0x80.
    int8_t prev = (int8_t)(out[0] = in[0] ^ in[len - 1]);
    for (size_t i = 1; i < len; i++) {
        int previous_hash_component = prev % 13;
        int input_byte_component = (int8_t)in[i] % 19;
        out[i] = (uint8_t)((previous_hash_component + 1) * (input_byte_component + 1));
        prev = (int8_t)out[i];
    }
}

size_t clu_pkcs7_pad(uint8_t *buf, size_t len)
{
    uint8_t padding_length = (uint8_t)(16 - (len % 16));
    for (uint8_t i = 0; i < padding_length; i++) {
        buf[len + i] = padding_length;
    }
    return len + padding_length;
}

int clu_pkcs7_unpad(const uint8_t *buf, size_t len)
{
    if (len == 0 || len % 16) {
        return -1;
    }
    uint8_t padding_length = buf[len - 1];
    if (padding_length == 0 || padding_length > 16) {
        return -1;
    }
    for (size_t i = len - padding_length; i < len; i++) {
        if (buf[i] != padding_length) {
            return -1;
        }
    }
    return (int)(len - padding_length);
}
