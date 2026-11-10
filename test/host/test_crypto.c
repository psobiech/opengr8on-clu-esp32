// Host test for components/clu_crypto (AES-128-CBC + PKCS#7 over the PSA API), linked against the
// system mbedtls 3.6 PSA implementation (same psa_* cipher calls as ESP-IDF's mbedtls 4).
// Vectors generated from the Node reference: dist/CipherKey.js encrypt/decrypt.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clu_crypto.h"
#include "clu_keys.h"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

static size_t unhex(uint8_t *out, const char *h)
{
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) {
        unsigned b;
        sscanf(h, "%2x", &b);
        out[n++] = (uint8_t)b;
    }
    return n;
}

static void check_enc(const char *name, const clu_cipher_t *c, const char *plain, const char *want_hex)
{
    uint8_t want[64], out[64], back[64];
    size_t wn = unhex(want, want_hex);
    int n = clu_encrypt(c, (const uint8_t *)plain, strlen(plain), out, sizeof out);
    check(name, n == (int)wn && memcmp(out, want, wn) == 0);
    int m = clu_decrypt(c, want, wn, back, sizeof back);
    char dn[96];
    snprintf(dn, sizeof dn, "%s (decrypt)", name);
    check(dn, m == (int)strlen(plain) && memcmp(back, plain, (size_t)m) == 0);
}

int main(void)
{
    check("psa init", clu_crypto_init() == ESP_OK);

    clu_cipher_t c;
    clu_default_key((const uint8_t *)"XYZW9876", c.key);
    unhex(c.iv, "00112233445566778899aabbccddeeff");
    check_enc("aes-cbc discovery request == Node", &c, "req_discovery_clu:192.168.1.50",
              "da6755ac93b3576c931f623519740625bbb2a3a8d4b07fd8bb47a68335c868b7");
    check_enc("aes-cbc empty -> one pad block", &c, "", "4bf66d7ace9c2018ec4f28c1b2ad63b0");
    check_enc("aes-cbc exact block -> extra pad block", &c, "0123456789abcdef",
              "32f897fb43c16f5357c23bf948cd76a9de183c73170a5e4b6eabcfc3b9abc73c");

    // Discovery hash on Java signed bytes (as OM and vCLU DiscoverCLUsCommand.hash): input
    // bytes (i*37+200)&0xff, mostly >= 0x80. Unsigned math gives a different hash and OM then
    // cannot verify the reply (asks for the PIN on every discovery).
    uint8_t hin[30], hout[30], hwant[30];
    for (int i = 0; i < 30; i++) {
        hin[i] = (uint8_t)(i * 37 + 200);
    }
    unhex(hwant, "310be4eebc1864743d1e0a0b941ec918482808ca07c063512031df18c42a");
    clu_discovery_hash(hin, 30, hout);
    check("discovery hash == Java signed-byte hash", memcmp(hout, hwant, 30) == 0);

    clu_cipher_t b;
    memcpy(b.key, CLU_GRENTON_KEY, 16);
    memcpy(b.iv, CLU_DEFAULT_BROADCAST_IV, 16);
    check_enc("aes-cbc broadcast key == Node", &b, "req_discovery_clu:192.168.1.50",
              "71e5a17a4c785a018b5c5c809d55886a49018170aa20817fbaedf9178b9cb45a");

    uint8_t ct[64], out[64];
    size_t n = unhex(ct, "da6755ac93b3576c931f623519740625bbb2a3a8d4b07fd8bb47a68335c868b7");
    check("decrypt with wrong key -> -1 (bad padding)", clu_decrypt(&b, ct, n, out, sizeof out) == -1);
    check("decrypt len % 16 -> -1", clu_decrypt(&c, ct, 31, out, sizeof out) == -1);
    check("decrypt empty -> -1", clu_decrypt(&c, ct, 0, out, sizeof out) == -1);
    check("decrypt cap < len -> -1", clu_decrypt(&c, ct, 32, out, 31) == -1);
    check("encrypt cap < len + 16 -> -1", clu_encrypt(&c, ct, 30, out, 45) == -1);

    static uint8_t big[1600], bigout[1700], bigback[1700];
    for (size_t i = 0; i < sizeof big; i++) {
        big[i] = (uint8_t)(i * 7);
    }
    check("encrypt > 1500 bytes -> -1", clu_encrypt(&c, big, 1501, bigout, sizeof bigout) == -1);
    int en = clu_encrypt(&c, big, 1500, bigout, sizeof bigout);
    int dn = en > 0 ? clu_decrypt(&c, bigout, (size_t)en, bigback, sizeof bigback) : -1;
    check("1500-byte round trip (max UDP payload)", en == 1504 && dn == 1500 && memcmp(big, bigback, 1500) == 0);

    uint8_t r1[16] = {0}, r2[16] = {0};
    clu_random(r1, sizeof r1);
    clu_random(r2, sizeof r2);
    check("clu_random fills and differs", memcmp(r1, r2, 16) != 0);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
