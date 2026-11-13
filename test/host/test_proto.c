// Host-side tests for clu_proto. Vectors generated from the Node reference
// implementation (dist/CipherKey.js, dist/commands/DiscoverCLUsCommand.js,
// dist/tfbus/TFBusCommands.js) and docs/TFBUS.md wire examples.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clu_keys.h"
#include "http_proto.h"
#include "tfbus_frame.h"
#include "tftp_proto.h"

static int failures;

static void hex(char *out, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", b[i]);
    }
    out[2 * n] = 0;
}

static void check_hex(const char *name, const uint8_t *b, size_t n, const char *expect)
{
    char got[256];
    hex(got, b, n);
    if (strcmp(got, expect) != 0) {
        printf("FAIL %s\n  got    %s\n  expect %s\n", name, got, expect);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

typedef struct {
    int n;
    uint8_t last[64];
    size_t last_len;
} rx_ctx_t;

static void on_frame(const uint8_t *f, size_t len, void *ctx)
{
    rx_ctx_t *c = ctx;
    c->n++;
    memcpy(c->last, f, len);
    c->last_len = len;
}

int main(void)
{
    uint8_t buf[64];
    size_t n;

    // --- keys --- (hash vector: Java signed-byte semantics, like OM)
    uint8_t r[30], h[30];
    for (int i = 0; i < 30; i++) {
        r[i] = (uint8_t)(i * 37 + 11);
    }
    clu_discovery_hash(r, 30, h);
    check_hex("discovery_hash", h, 30,
              "372c3c51fc06eb97000d0c230a001266c08f7079b0feff00f66394270518");

    uint8_t k[16];
    clu_default_key((const uint8_t *)"ABCD1234", k);
    check_hex("default_key(ABCD1234)", k, 16, "f8eda0c3c82b90e794e4a8efca52e357");

    uint8_t pad[48] = "req_discovery_clu:192.168.1.50";
    n = clu_pkcs7_pad(pad, 30);
    check("pkcs7_pad 30->32", n == 32 && pad[30] == 2 && pad[31] == 2);
    check("pkcs7_unpad", clu_pkcs7_unpad(pad, n) == 30);
    pad[31] = 3;
    check("pkcs7_unpad rejects bad", clu_pkcs7_unpad(pad, n) == -1);

    // --- tfbus tx ---
    n = tfbus_reset(buf, sizeof buf, TFBUS_BROADCAST);
    check_hex("reset bcast", buf, n, "aaff0752");
    n = tfbus_seek(buf, sizeof buf);
    check_hex("seek", buf, n, "aaff2a7f");
    n = tfbus_addr_assign(buf, sizeof buf, 0x12345678, 3);
    check_hex("addr_assign", buf, n, "aaff0278563412035c");
    n = tfbus_version_query(buf, sizeof buf, 7);
    check_hex("version_query", buf, n, "aa07f954");

    uint8_t lvl = 0x80;
    tfbus_tlv_t act = {.type = 0x08, .meta = 0x01, .len = 1, .data = &lvl};
    n = tfbus_module_cmd(buf, sizeof buf, 3, &act, 1);
    check_hex("module_cmd ACTIVATE", buf, n, "aa03f0060108010180d6");

    uint8_t pos[3] = {0, 3, 41};
    tfbus_tlv_t sp = {.type = 0x16, .meta = 0x00, .len = 3, .data = pos};
    n = tfbus_module_cmd(buf, sizeof buf, 7, &sp, 1);
    check_hex("module_cmd SET_POSITION", buf, n, "aa07f008011603000003296b");

    n = tfbus_cmd(buf, sizeof buf, 5, 0x00, NULL, 0);
    check_hex("tfbus_cmd CHECK_ALIVE", buf, n,
              "aa0531000000000000000000000000000000000000000000009e");

    // --- tfbus rx ---
    const uint8_t ack[] = {0xaa, 0x00, 0x01, 0xab};
    const uint8_t *pl;
    size_t plen;
    check("parse ack", tfbus_parse_response(ack, 4, &pl, &plen) && plen == 0);

    // DIGITAL 1ch: 01 06 01 01 [01]  -> len byte = 5+1
    uint8_t resp[16] = {0xaa, 0x00, 0x06, 0x01, 0x06, 0x01, 0x01, 0x01};
    resp[8] = tfbus_xor(resp, 8);
    check("parse status resp", tfbus_parse_response(resp, 9, &pl, &plen) && plen == 5);
    tfbus_tlv_t recs[4];
    int nr = tfbus_parse_tlv(pl, plen, recs, 4);
    check("tlv 1 rec", nr == 1 && recs[0].type == 0x06 && recs[0].meta == 0x01 &&
                           recs[0].len == 1 && recs[0].data[0] == 0x01);
    check("tlv malformed", tfbus_parse_tlv((const uint8_t[]){0x02, 0x06, 0x01, 0x01, 0x01}, 5, recs, 4) == -1);

    // streaming: garbage + ack + module_cmd split in chunks
    tfbus_rx_t rx;
    rx_ctx_t c = {0};
    tfbus_rx_init(&rx);
    const uint8_t stream[] = {0x13, 0x37, 0xaa, 0x00, 0x01, 0xab,
                              0xaa, 0x03, 0xf0, 0x06, 0x01, 0x08, 0x01, 0x01, 0x80, 0xd6};
    tfbus_rx_push(&rx, stream, 5, on_frame, &c);
    check("rx partial -> 0 frames", c.n == 0);
    tfbus_rx_push(&rx, stream + 5, 6, on_frame, &c);
    check("rx ack", c.n == 1 && c.last_len == 4);
    tfbus_rx_push(&rx, stream + 11, sizeof stream - 11, on_frame, &c);
    check("rx module_cmd", c.n == 2 && c.last_len == 10 && c.last[2] == 0xf0);

    // --- frame limits, like real CLU: payload <= 28, frame <= 32 (TX and RX) ---
    {
        uint8_t pl[32] = {0}, f[64];
        check("tx payload 28 ok", tfbus_build(f, sizeof f, 1, 0xf0, pl, 28) == 32);
        check("tx payload 29 rejected", tfbus_build(f, sizeof f, 1, 0xf0, pl, 29) == 0);

        // module reply: AA 00 len payload[len-1] xor, frame = len + 3
        for (int plen = 28; plen <= 29; plen++) {
            uint8_t r[40] = {0xaa, 0x00, (uint8_t)(plen + 1)};
            for (int i = 0; i < plen; i++) {
                r[3 + i] = (uint8_t)(0x40 + i);
            }
            r[3 + plen] = tfbus_xor(r, 3 + plen);
            tfbus_rx_init(&rx);
            c.n = 0;
            tfbus_rx_push(&rx, r, 4 + plen, on_frame, &c);
            if (plen == 28) {
                check("rx payload 28 (frame 32) accepted", c.n == 1 && c.last_len == 32);
            } else {
                check("rx payload 29 (frame 33) rejected", c.n == 0 || c.last_len != 33);
            }
        }
    }

    // --- tftp ---
    char path[64];
    check("tftp a:\\MAIN.LUA", tftp_resolve_path("/fs", "a:\\MAIN.LUA", path, sizeof path) &&
                                   strcmp(path, "/fs/a/MAIN.LUA") == 0);
    check("tftp A:/om.lua", tftp_resolve_path("/fs", "A:/om.lua", path, sizeof path) &&
                                strcmp(path, "/fs/a/om.lua") == 0);
    check("tftp p:\\meas.bin", tftp_resolve_path("/fs", "p:\\meas.bin", path, sizeof path) &&
                                   strcmp(path, "/fs/p/meas.bin") == 0);
    check("tftp bare name -> a", tftp_resolve_path("/fs", "USER.LUA", path, sizeof path) &&
                                     strcmp(path, "/fs/a/USER.LUA") == 0);
    check("tftp /a/x", tftp_resolve_path("/fs", "/a/CONFIG.JSON", path, sizeof path) &&
                           strcmp(path, "/fs/a/CONFIG.JSON") == 0);
    check("tftp rejects ..", !tftp_resolve_path("/fs", "a:\\..\\x", path, sizeof path));
    check("tftp rejects nested", !tftp_resolve_path("/fs", "a:\\d\\x", path, sizeof path));
    check("tftp rejects empty", !tftp_resolve_path("/fs", "a:\\", path, sizeof path));

    const uint8_t rrq[] = {0, 1, 'a', ':', '\\', 'O', 'M', '.', 'L', 'U', 'A', 0, 'o', 'c', 't', 'e', 't', 0};
    uint16_t op;
    const char *fn, *md;
    check("tftp parse RRQ", tftp_parse_request(rrq, sizeof rrq, &op, &fn, &md) && op == 1 &&
                                strcmp(fn, "a:\\OM.LUA") == 0 && strcmp(md, "octet") == 0);
    check("tftp parse truncated", !tftp_parse_request(rrq, 10, &op, &fn, &md));

    uint8_t na_out[16];
    bool pcr = false;
    size_t nl = tftp_netascii_decode((const uint8_t *)"a\r", 2, na_out, &pcr);
    nl += tftp_netascii_decode((const uint8_t *)"\nb\r\0c", 5, na_out + nl, &pcr);
    check("netascii decode across chunks", nl == 5 && memcmp(na_out, "a\nb\rc", 5) == 0);

    // --- keys: more vectors (Node dist/CipherKey.js generateKey / DEFAULT_BROADCAST) ---
    {
        const uint8_t iv[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
        clu_derive_key(iv, (const uint8_t *)"XYZW9876", k);
        check_hex("derive_key(iv, XYZW9876) == Node generateKey", k, 16,
                  "584878647d6d5141beae92829b87b7a7");
        check_hex("GRENTON_KEY == base64 hd5SHpxl0N5+WEXTXlPQmw==", CLU_GRENTON_KEY, 16,
                  "85de521e9c65d0de7e5845d35e53d09b");
        check_hex("DEFAULT_BROADCAST_IV", CLU_DEFAULT_BROADCAST_IV, 16,
                  "07060504030201000402090304010507");

        uint8_t blk[48] = {0};
        n = clu_pkcs7_pad(blk, 16);
        check("pkcs7_pad full block adds 16x0x10", n == 32 && blk[16] == 16 && blk[31] == 16);
        check("pkcs7_unpad full block", clu_pkcs7_unpad(blk, 32) == 16);
        check("pkcs7_pad empty", clu_pkcs7_pad(blk, 0) == 16 && clu_pkcs7_unpad(blk, 16) == 0);
        blk[15] = 0;
        check("pkcs7_unpad rejects pad 0", clu_pkcs7_unpad(blk, 16) == -1);
        blk[15] = 17;
        check("pkcs7_unpad rejects pad > 16", clu_pkcs7_unpad(blk, 16) == -1);
        check("pkcs7_unpad rejects len % 16", clu_pkcs7_unpad(blk, 15) == -1 && clu_pkcs7_unpad(blk, 0) == -1);
    }

    // --- tfbus tx: remaining builders and error paths ---
    n = tfbus_status_poll(buf, sizeof buf, 3);
    check_hex("status_poll", buf, n, "aa0304ad");
    n = tfbus_addr_confirm(buf, sizeof buf, 3);
    check_hex("addr_confirm", buf, n, "aa030902a2");
    n = tfbus_reset(buf, sizeof buf, 5);
    check_hex("reset unicast", buf, n, "aa0507a8");
    check("build: cap too small -> 0", tfbus_build(buf, 3, 1, 0x04, NULL, 0) == 0);
    {
        uint8_t big[24] = {0};
        tfbus_tlv_t r = {.type = 0x01, .meta = 0, .len = 24, .data = big};
        check("module_cmd: record overflows payload -> 0", tfbus_module_cmd(buf, sizeof buf, 1, &r, 1) == 0);
        check("module_cmd: no records -> 0", tfbus_module_cmd(buf, sizeof buf, 1, &r, 0) == 0);
        uint8_t d[30];
        memset(d, 0x11, sizeof d);
        n = tfbus_cmd(buf, sizeof buf, 5, 0x07, d, sizeof d);
        check("tfbus_cmd: data truncated to 21 bytes", n == 26 && buf[3] == 0x07 && buf[24] == 0x11);
    }

    // --- tfbus rx: response validation ---
    {
        uint8_t f[8] = {0xaa, 0x00, 0x02, 0x05, 0};
        f[4] = tfbus_xor(f, 4);
        check("parse resp 1-byte payload", tfbus_parse_response(f, 5, &pl, &plen) && plen == 1 && pl[0] == 5);
        f[4] ^= 1;
        check("parse resp bad xor", !tfbus_parse_response(f, 5, &pl, &plen));
        f[4] ^= 1;
        check("parse resp truncated", !tfbus_parse_response(f, 4, &pl, &plen));
        f[1] = 0x03;
        check("parse resp dest != CLU", !tfbus_parse_response(f, 5, &pl, &plen));
        const uint8_t big[] = {0xaa, 0x00, 0x1e};
        check("parse resp len byte > 0x1d", !tfbus_parse_response(big, 3, &pl, &plen));
    }

    // --- tlv: count/length mismatches, max truncation ---
    {
        const uint8_t three[] = {3, 0x06, 1, 1, 1, 0x07, 1, 1, 2, 0x08, 1, 1, 3};
        tfbus_tlv_t rr[2];
        check("tlv max truncates to 2", tfbus_parse_tlv(three, sizeof three, rr, 2) == 2 &&
                                            rr[1].type == 0x07 && rr[1].data[0] == 2);
        check("tlv trailing bytes -> -1", tfbus_parse_tlv((const uint8_t[]){1, 6, 1, 1, 1, 9}, 6, rr, 2) == -1);
        check("tlv count 0", tfbus_parse_tlv((const uint8_t[]){0}, 1, rr, 2) == 0);
        check("tlv empty payload", tfbus_parse_tlv(NULL, 0, rr, 2) == 0);
    }

    // --- rx streaming: resync and module replies whose length byte collides with a frame type ---
    {
        // corrupted frame (bad xor) followed by a valid ACK
        const uint8_t bad[] = {0xaa, 0x00, 0x01, 0x00, 0xaa, 0x00, 0x01, 0xab};
        tfbus_rx_init(&rx);
        c.n = 0;
        tfbus_rx_push(&rx, bad, sizeof bad, on_frame, &c);
        check("rx resync after bad xor -> ACK", c.n == 1 && c.last_len == 4 && c.last[3] == 0xab);

        // 200 bytes of noise without 0xAA, then ACK: buffer must not wedge
        uint8_t noise[204];
        memset(noise, 0x55, 200);
        memcpy(noise + 200, (const uint8_t[]){0xaa, 0x00, 0x01, 0xab}, 4);
        tfbus_rx_init(&rx);
        c.n = 0;
        tfbus_rx_push(&rx, noise, sizeof noise, on_frame, &c);
        check("rx noise > buffer then ACK", c.n == 1 && c.last_len == 4);

        // Module -> CLU reply (dest 0x00) is framed by its length byte only. len byte 0x02 is
        // also the ADDR_ASSIGN type (fixed 9 bytes): the 5-byte reply must not wait for 9 bytes
        // (tfbus_uart_transact gets no more bytes after the module's reply).
        uint8_t r2[5] = {0xaa, 0x00, 0x02, 0x00};
        r2[4] = tfbus_xor(r2, 4);
        tfbus_rx_init(&rx);
        c.n = 0;
        tfbus_rx_push(&rx, r2, sizeof r2, on_frame, &c);
        check("rx reply len 0x02 (5 bytes) delivered", c.n == 1 && c.last_len == 5);

        // len byte 0x04 (= STATUS_POLL type, 4 bytes) with payload[0] == xor(aa 00 04) = 0xae
        uint8_t r4[7] = {0xaa, 0x00, 0x04, 0xae, 0x01, 0x02};
        r4[6] = tfbus_xor(r4, 6);
        tfbus_rx_init(&rx);
        c.n = 0;
        tfbus_rx_push(&rx, r4, sizeof r4, on_frame, &c);
        check("rx reply len 0x04 not split at 4 bytes", c.n == 1 && c.last_len == 7);

        // CLU -> module frames still use the typed table (bus sniffing / own echo)
        tfbus_rx_init(&rx);
        c.n = 0;
        n = tfbus_addr_assign(buf, sizeof buf, 0x12345678, 3);
        tfbus_rx_push(&rx, buf, n, on_frame, &c);
        check("rx ADDR_ASSIGN typed length 9", c.n == 1 && c.last_len == 9);
    }

    // --- tftp: packet builders, path edge cases ---
    {
        uint8_t t[16];
        n = tftp_ack(t, 0x1234);
        check_hex("tftp ack", t, n, "00041234");
        n = tftp_data_hdr(t, 513);
        check_hex("tftp data hdr", t, n, "00030201");
        n = tftp_error(t, sizeof t, TFTP_E_NOT_FOUND, "nope");
        check_hex("tftp error", t, n, "000500016e6f706500");
        n = tftp_error(t, 8, TFTP_E_ACCESS, "long message");
        check("tftp error truncated to cap", n == 8 && t[7] == 0 && memcmp(t + 4, "lon", 3) == 0);
        check("tftp error cap < 5", tftp_error(t, 4, 0, "x") == 0);

        check("tftp rejects drive colon in name", !tftp_resolve_path("/fs", "a:\\x:y", path, sizeof path));
        check("tftp rejects control char", !tftp_resolve_path("/fs", "a:\\x\ty", path, sizeof path));
        check("tftp rejects '.'", !tftp_resolve_path("/fs", "a:/.", path, sizeof path));
        check("tftp out buffer too small", !tftp_resolve_path("/fs", "a:\\MAIN.LUA", path, 10));
        check("tftp P: uppercase drive", tftp_resolve_path("/fs", "P:\\X", path, sizeof path) &&
                                             strcmp(path, "/fs/p/X") == 0);

        const uint8_t nomode[] = {0, 2, 'x', 0};
        check("tftp parse missing mode", !tftp_parse_request(nomode, sizeof nomode, &op, &fn, &md));
        const uint8_t noname[] = {0, 1, 0, 'o', 0};
        check("tftp parse empty filename", !tftp_parse_request(noname, sizeof noname, &op, &fn, &md));
        const uint8_t unterminated[] = {0, 1, 'x', 0, 'o', 'c'};
        check("tftp parse unterminated mode",
              !tftp_parse_request(unterminated, sizeof unterminated, &op, &fn, &md));

        pcr = false;
        nl = tftp_netascii_decode((const uint8_t *)"a\r\nb\rx", 6, na_out, &pcr);
        check("netascii CRLF -> LF, lone CR kept", nl == 5 && memcmp(na_out, "a\nb\rx", 5) == 0 && !pcr);
        pcr = false;
        nl = tftp_netascii_decode((const uint8_t *)"z\r", 2, na_out, &pcr);
        check("netascii trailing CR pending", nl == 1 && pcr);
    }

    // ---- HTTP request head (GATE HttpListener) ----
    {
        char b[512];
        http_req_head_t h;
        strcpy(b, "GET /api/x?a=1&b=2 HTTP/1.1\r\nHost: gate\r\nX-A:  v  \r\n\r\nrest");
        int n = http_parse_head(b, strlen(b), &h);
        check("http head length", n == (int)strlen("GET /api/x?a=1&b=2 HTTP/1.1\r\nHost: gate\r\nX-A:  v  \r\n\r\n"));
        check("http method / path / query", strcmp(h.method, "GET") == 0 && strcmp(h.path, "/api/x") == 0 &&
                                                strcmp(h.query, "a=1&b=2") == 0);
        check("http headers block", strcmp(h.headers, "Host: gate\r\nX-A:  v  \r\n") == 0);
        check("http no body", h.content_length == 0 && !h.chunked);
        check("http body after head", strcmp(b + n, "rest") == 0);

        strcpy(b, "POST /p HTTP/1.0\r\ncontent-length: 12\r\nTransfer-Encoding: gzip, chunked\r\n\r\n");
        n = http_parse_head(b, strlen(b), &h);
        check("http content-length (any case) / chunked", n > 0 && h.content_length == 12 && h.chunked &&
                                                              strcmp(h.query, "") == 0);
        strcpy(b, "GET / HTTP/1.1\r\n\r\n");
        n = http_parse_head(b, strlen(b), &h);
        check("http no headers", n == 18 && strcmp(h.path, "/") == 0 && h.headers[0] == 0);
        strcpy(b, "GET http://gate:80/x/y?q HTTP/1.1\r\n\r\n");
        check("http absolute-form target", http_parse_head(b, strlen(b), &h) > 0 && strcmp(h.path, "/x/y") == 0 &&
                                               strcmp(h.query, "q") == 0);
        strcpy(b, "GET / HTTP/1.1\r\nHost: x\r\n");
        check("http incomplete head", http_parse_head(b, strlen(b), &h) == 0);
        strcpy(b, "get / HTTP/1.1\r\n\r\n");
        check("http lowercase method rejected", http_parse_head(b, strlen(b), &h) == -1);
        strcpy(b, "GET / FTP/1.1\r\n\r\n");
        check("http bad version rejected", http_parse_head(b, strlen(b), &h) == -1);
        strcpy(b, "GET /\r\n\r\n");
        check("http missing version rejected", http_parse_head(b, strlen(b), &h) == -1);
        strcpy(b, "POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n");
        check("http bad content-length rejected", http_parse_head(b, strlen(b), &h) == -1);
        strcpy(b, "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n");
        check("http negative content-length rejected", http_parse_head(b, strlen(b), &h) == -1);
        memcpy(b, "GET /\0x HTTP/1.1\r\n", 18);
        check("http NUL in head rejected", http_parse_head(b, 18, &h) == -1);

        char out[256];
        n = http_format_head(out, sizeof out, 404, "Content-Type: text/plain\r\n", 9);
        check("http response head", n > 0 && strcmp(out, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                                                          "Content-Length: 9\r\nConnection: close\r\n\r\n") == 0);
        n = http_format_head(out, sizeof out, 42, NULL, 0);
        check("http bad status -> 500", n > 0 && strncmp(out, "HTTP/1.1 500 Internal Server Error\r\n", 36) == 0);
        check("http head too small", http_format_head(out, 20, 200, NULL, 0) == -1);
        check("http reason unknown", strcmp(http_reason(299), "Status") == 0);
    }

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
