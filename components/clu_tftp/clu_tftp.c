/**
 * @file clu_tftp.c
 * @brief TFTP server for the CLU drives (RFC 1350, 512-byte blocks, octet and netascii).
 *
 * One transfer at a time, served on its own ephemeral socket (new TID). Only the peer that enabled
 * serving is accepted; uploads are written to TFTP.TMP and swapped in only when complete.
 */
#include "clu_tftp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "tftp_proto.h"

static const char *TAG = "tftp";

#define TIMEOUT_MS  5000
#define MAX_RETRIES 3
#define PKT_MAX     (4 + TFTP_BLOCK_SIZE)

static char s_root[16];
static volatile bool s_enabled;
static volatile uint32_t s_peer_ip;   // network order
static volatile int64_t s_idle_until; // us
static clu_tftp_upload_cb_t s_upload_cb;

/** @brief Restart the idle timer (serving stops CLU_TFTP_IDLE_S after the last activity). */
static void touch(void)
{
    s_idle_until = esp_timer_get_time() + (int64_t)CLU_TFTP_IDLE_S * 1000000;
}

void clu_tftp_enable(bool on, uint32_t peer_ip)
{
    if (s_enabled != on) {
        ESP_LOGI(TAG, "%s", on ? "enabled" : "disabled");
    }
    s_peer_ip = on ? peer_ip : 0;
    touch();
    s_enabled = on;
}

bool clu_tftp_enabled(void)
{
    if (s_enabled && esp_timer_get_time() > s_idle_until) {
        ESP_LOGI(TAG, "disabled (idle %d s)", CLU_TFTP_IDLE_S);
        s_enabled = false;
    }
    return s_enabled;
}

void clu_tftp_set_upload_cb(clu_tftp_upload_cb_t cb)
{
    s_upload_cb = cb;
}

/** @brief Send a TFTP ERROR packet. */
static void send_err(int sock, const struct sockaddr_in *to, uint16_t code, const char *msg)
{
    uint8_t b[64];
    size_t n = tftp_error(b, sizeof b, code, msg);
    sendto(sock, b, n, 0, (const struct sockaddr *)to, sizeof *to);
}

/** @brief esp_timer time @p ms from now, in microseconds. */
static int64_t deadline_in(int ms)
{
    return esp_timer_get_time() + (int64_t)ms * 1000;
}

// Wait until deadline_us (esp_timer time) for a packet from the peer (addr+port). Stray TIDs
// get an error (the first few only: no reflector) and do not extend the wait, so a flood from
// another host cannot hold the single transfer slot.
#define STRAY_ERRORS 4
/**
 * @brief Wait until deadline_us (esp_timer time) for a packet from the peer (addr+port).
 *
 * Stray TIDs get an error (the first few only: no reflector) and do not extend the wait, so a
 * flood from another host cannot hold the single transfer slot.
 * @return Packet length, or <= 0 on timeout / error.
 */
static int recv_from_peer(int sock, uint8_t *buf, size_t cap, const struct sockaddr_in *peer,
                          int64_t deadline_us)
{
    for (int stray = 0;;) {
        int64_t left = deadline_us - esp_timer_get_time();
        if (left <= 0) {
            return -1;
        }
        if (left < 1000) {
            left = 1000; // lwip: tv 0 = block forever
        }
        struct timeval tv = {.tv_sec = left / 1000000, .tv_usec = left % 1000000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(sock, buf, cap, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            return -1; // timeout
        }
        if (from.sin_addr.s_addr != peer->sin_addr.s_addr || from.sin_port != peer->sin_port) {
            if (stray++ < STRAY_ERRORS) {
                send_err(sock, &from, TFTP_E_UNKNOWN_TID, "unknown TID");
            }
            continue;
        }
        return n;
    }
}

/**
 * @brief Open a UDP socket on an ephemeral port (the transfer TID) with a receive timeout; -1 on error.
 */
static int open_transfer_socket(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return -1;
    }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_ANY)};
    struct timeval tv = {.tv_sec = TIMEOUT_MS / 1000, .tv_usec = (TIMEOUT_MS % 1000) * 1000};
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 ||
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0) {
        close(s);
        return -1;
    }
    return s;
}

/**
 * @brief Netascii encode (LF -> CR LF, CR -> CR NUL). out must hold 2*len.
 * @return Bytes written.
 */
static size_t netascii_encode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '\n') {
            out[o++] = '\r';
            out[o++] = '\n';
        } else if (in[i] == '\r') {
            out[o++] = '\r';
            out[o++] = 0;
        } else {
            out[o++] = in[i];
        }
    }
    return o;
}

/**
 * @brief Serve a read request: send the file block by block, retransmitting on ACK timeout.
 * @param peer     Requesting host (addr + port).
 * @param path     Resolved VFS path.
 * @param netascii Convert LF to CR LF on the wire.
 */
static void serve_rrq(const struct sockaddr_in *peer, const char *path, bool netascii)
{
    int sock = open_transfer_socket();
    if (sock < 0) {
        return;
    }
    uint8_t *file = NULL;
    size_t flen = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        send_err(sock, peer, TFTP_E_NOT_FOUND, "file not found");
        goto out;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        fclose(f);
        send_err(sock, peer, TFTP_E_UNDEFINED, "stat failed");
        goto out;
    }
    // Whole file in PSRAM (internal RAM without it): CLU files are small and netascii needs lookahead-free conversion.
    size_t raw_len = (size_t)st.st_size;
    uint8_t *raw = heap_caps_malloc(raw_len ? raw_len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) {
        raw = malloc(raw_len ? raw_len : 1);
    }
    if (!raw || fread(raw, 1, raw_len, f) != raw_len) {
        fclose(f);
        free(raw);
        send_err(sock, peer, TFTP_E_UNDEFINED, "read failed");
        goto out;
    }
    fclose(f);
    if (netascii) {
        file = heap_caps_malloc(raw_len * 2 + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!file) {
            file = malloc(raw_len * 2 + 1);
        }
        if (!file) {
            free(raw);
            send_err(sock, peer, TFTP_E_UNDEFINED, "out of memory");
            goto out;
        }
        flen = netascii_encode(raw, raw_len, file);
        free(raw);
    } else {
        file = raw;
        flen = raw_len;
    }

    uint8_t pkt[PKT_MAX], rx[PKT_MAX];
    uint16_t block = 1;
    size_t off = 0;
    for (;;) {
        size_t chunk = flen - off < TFTP_BLOCK_SIZE ? flen - off : TFTP_BLOCK_SIZE;
        size_t pl = tftp_data_hdr(pkt, block);
        memcpy(pkt + pl, file + off, chunk);
        pl += chunk;

        bool acked = false;
        for (int tries = 0; tries <= MAX_RETRIES && !acked; tries++) {
            sendto(sock, pkt, pl, 0, (const struct sockaddr *)peer, sizeof *peer);
            int n;
            int64_t until = deadline_in(TIMEOUT_MS); // duplicate ACKs do not extend it either
            while ((n = recv_from_peer(sock, rx, sizeof rx, peer, until)) >= 4) {
                uint16_t op = (uint16_t)(rx[0] << 8 | rx[1]);
                uint16_t b = (uint16_t)(rx[2] << 8 | rx[3]);
                if (op == TFTP_ERROR) {
                    ESP_LOGW(TAG, "RRQ %s aborted by peer", path);
                    goto out;
                }
                if (op == TFTP_ACK && b == block) {
                    acked = true;
                    break;
                }
                // duplicate/old ACK: keep waiting for the right one
            }
        }
        if (!acked) {
            ESP_LOGW(TAG, "RRQ %s timed out at block %u", path, block);
            goto out;
        }
        off += chunk;
        block++;
        if (chunk < TFTP_BLOCK_SIZE) {
            break;
        }
    }
    ESP_LOGI(TAG, "sent %s (%u bytes)", path, (unsigned)flen);

out:
    free(file);
    close(sock);
}

/**
 * @brief Serve a write request into @p path (via TFTP.TMP), up to CLU_TFTP_MAX_UPLOAD bytes.
 *
 * Calls the upload callback on success.
 * @param peer     Sending host (addr + port).
 * @param path     Resolved VFS path.
 * @param netascii Decode CR LF / CR NUL on the wire.
 */
static void serve_wrq(const struct sockaddr_in *peer, const char *path, bool netascii)
{
    int sock = open_transfer_socket();
    if (sock < 0) {
        return;
    }

    // Write next to target, then swap, so a failed upload never truncates the old file.
    char tmp[64];
    const char *slash = strrchr(path, '/');
    snprintf(tmp, sizeof tmp, "%.*s/TFTP.TMP", (int)(slash - path), path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        send_err(sock, peer, TFTP_E_ACCESS, "cannot create file");
        close(sock);
        return;
    }

    uint8_t pkt[PKT_MAX], dec[PKT_MAX];
    size_t total = 0;
    bool pending_cr = false;
    bool ok = false;
    uint16_t expect = 1;

    size_t al = tftp_ack(pkt, 0);
    sendto(sock, pkt, al, 0, (const struct sockaddr *)peer, sizeof *peer);

    int retries = 0;
    int64_t until = deadline_in(TIMEOUT_MS); // renewed only on progress or a retry
    for (;;) {
        int n = recv_from_peer(sock, pkt, sizeof pkt, peer, until);
        if (n < 0) {
            if (++retries > MAX_RETRIES) {
                ESP_LOGW(TAG, "WRQ %s timed out at block %u", path, expect);
                break;
            }
            until = deadline_in(TIMEOUT_MS);
            uint8_t a[4];
            tftp_ack(a, expect - 1);
            sendto(sock, a, 4, 0, (const struct sockaddr *)peer, sizeof *peer);
            continue;
        }
        if (n < 4) {
            continue;
        }
        uint16_t op = (uint16_t)(pkt[0] << 8 | pkt[1]);
        uint16_t b = (uint16_t)(pkt[2] << 8 | pkt[3]);
        if (op == TFTP_ERROR) {
            ESP_LOGW(TAG, "WRQ %s aborted by peer", path);
            break;
        }
        if (op != TFTP_DATA) {
            send_err(sock, peer, TFTP_E_ILLEGAL, "expected DATA");
            break;
        }
        uint8_t a[4];
        if (b != expect) {
            // retransmission of previous block: re-ACK it
            tftp_ack(a, b);
            sendto(sock, a, 4, 0, (const struct sockaddr *)peer, sizeof *peer);
            continue;
        }
        retries = 0;
        until = deadline_in(TIMEOUT_MS);
        size_t dl = (size_t)n - 4;
        const uint8_t *data = pkt + 4;
        size_t wl = dl;
        if (netascii) {
            wl = tftp_netascii_decode(data, dl, dec, &pending_cr);
            data = dec;
        }
        if (total + wl > CLU_TFTP_MAX_UPLOAD) {
            send_err(sock, peer, TFTP_E_DISK_FULL, "file too large");
            break;
        }
        if (fwrite(data, 1, wl, f) != wl) {
            send_err(sock, peer, TFTP_E_DISK_FULL, "write failed");
            break;
        }
        total += wl;
        tftp_ack(a, b);
        sendto(sock, a, 4, 0, (const struct sockaddr *)peer, sizeof *peer);
        expect++;
        if (dl < TFTP_BLOCK_SIZE) {
            if (pending_cr) {
                fputc('\r', f);
                total++;
            }
            ok = true;
            break;
        }
    }

    ok = (fclose(f) == 0) && ok;
    if (ok) {
        unlink(path); // FAT rename() fails when the target exists
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        unlink(tmp);
        ESP_LOGW(TAG, "upload of %s failed (errno %d)", path, errno);
    } else {
        ESP_LOGI(TAG, "received %s (%u bytes)", path, (unsigned)total);
        if (s_upload_cb) {
            s_upload_cb(path);
        }
    }
    close(sock);
}

/** @brief TFTP task: accept RRQ/WRQ on udp/69 and serve them one at a time. */
static void tftp_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(TFTP_PORT),
                            .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "bind udp/%d failed", TFTP_PORT);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on udp/%d (root %s)", TFTP_PORT, s_root);

    uint8_t req[PKT_MAX];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(sock, req, sizeof req - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            continue;
        }
        req[n] = 0;
        uint16_t op;
        const char *fn, *mode;
        if (!tftp_parse_request(req, (size_t)n, &op, &fn, &mode) ||
            (op != TFTP_RRQ && op != TFTP_WRQ)) {
            send_err(sock, &from, TFTP_E_ILLEGAL, "illegal operation");
            continue;
        }
        if (!clu_tftp_enabled()) {
            send_err(sock, &from, TFTP_E_ACCESS, "tftp disabled");
            continue;
        }
        if (from.sin_addr.s_addr != s_peer_ip) {
            send_err(sock, &from, TFTP_E_ACCESS, "access denied");
            continue;
        }
        char path[64];
        if (!tftp_resolve_path(s_root, fn, path, sizeof path)) {
            send_err(sock, &from, TFTP_E_ACCESS, "access denied");
            continue;
        }
        bool netascii = strcasecmp(mode, "netascii") == 0;
        ESP_LOGI(TAG, "%s %s -> %s (%s)", op == TFTP_RRQ ? "RRQ" : "WRQ", fn, path, mode);
        if (op == TFTP_RRQ) {
            serve_rrq(&from, path, netascii);
        } else {
            serve_wrq(&from, path, netascii);
        }
        touch(); // idle timer counts from the end of the last transfer
    }
}

void clu_tftp_init(const char *root)
{
    strlcpy(s_root, root, sizeof s_root);
    xTaskCreatePinnedToCore(tftp_task, "clu_tftp", 6144, NULL, 4, NULL, 0);
}
