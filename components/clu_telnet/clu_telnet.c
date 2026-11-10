/**
 * @file clu_telnet.c
 * @brief Read-only log console over TCP (telnet / nc).
 *
 * A vprintf hook copies every log line into a RAM ring buffer (CR LF line ends); a server task
 * streams it to each client from its own read position. Slow clients lose the overwritten part
 * and get a notice.
 */
#include "clu_telnet.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "telnet";

#define LOG_LINE_MAX  256 // one formatted log line (on the caller's stack); longer lines are cut
#define CHUNK     512
#define POLL_MS   50

/** @brief Connected client. */
typedef struct {
    int fd;
    uint64_t rd;   // absolute ring position sent so far
    uint8_t iac;   // telnet command parser state (see client_read)
    char ip[16];
} client_t;

static clu_telnet_cfg_t s_cfg;
static uint32_t s_allow_ip, s_allow_mask; // network order; mask 0 = any
static vprintf_like_t s_orig;
static uint8_t *s_buf;
static size_t s_size;
static uint64_t s_head; // total bytes ever written
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static client_t *s_cli;

// ---- ring buffer (callers: any task, inside the log call) ----------------------------------

/** @brief Append to the ring buffer (LF -> CR LF). Any task, inside the log call. */
static void ring_put(const char *s, size_t n)
{
    taskENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) {
            s_buf[s_head++ % s_size] = '\r'; // telnet wants CRLF
        }
        s_buf[s_head++ % s_size] = (uint8_t)s[i];
    }
    taskEXIT_CRITICAL(&s_lock);
}

/**
 * @brief vprintf hook: print as before (UART) and copy the formatted line into the ring buffer.
 */
static int log_hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int r = s_orig ? s_orig(fmt, ap) : vprintf(fmt, ap);
    char line[LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        if (n >= (int)sizeof line) {
            memcpy(line + sizeof line - 5, "...\n", 4);
            n = sizeof line - 1;
        }
        ring_put(line, n);
    }
    return r;
}

/**
 * @brief Copy up to cap bytes from absolute position *rd.
 *
 * @return Bytes copied; *lost = bytes the reader fell behind (overwritten before it got them),
 *         *rd is advanced past them.
 */
static size_t ring_get(uint64_t *rd, uint8_t *dst, size_t cap, uint64_t *lost)
{
    taskENTER_CRITICAL(&s_lock);
    uint64_t head = s_head;
    uint64_t oldest = head > s_size ? head - s_size : 0;
    *lost = 0;
    if (*rd < oldest) {
        *lost = oldest - *rd;
        *rd = oldest;
    }
    size_t n = head - *rd < cap ? (size_t)(head - *rd) : cap;
    for (size_t i = 0; i < n; i++) {
        dst[i] = s_buf[(*rd + i) % s_size];
    }
    *rd += n;
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

// ---- TCP server ----------------------------------------------------------------------------

/** @brief Send the whole buffer; false on socket error. */
static bool send_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        int w = send(fd, b, n, 0);
        if (w <= 0) {
            return false;
        }
        b += w;
        n -= w;
    }
    return true;
}

/** @brief Close a client session (logged with @p why). */
static void client_close(client_t *c, const char *why)
{
    ESP_LOGI(TAG, "client %s closed: %s", c->ip, why);
    close(c->fd);
    c->fd = -1;
}

/**
 * @brief Accept a connection on @p ls: check the allow list and free slots, start from the buffered history.
 */
static void client_accept(int ls)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int fd = accept(ls, (struct sockaddr *)&a, &al);
    if (fd < 0) {
        return;
    }
    char ip[16];
    inet_ntoa_r(a.sin_addr, ip, sizeof ip);
    if ((a.sin_addr.s_addr & s_allow_mask) != (s_allow_ip & s_allow_mask)) {
        ESP_LOGW(TAG, "refused %s (CLU_TELNET_ALLOW)", ip);
        close(fd);
        return;
    }
    client_t *c = NULL;
    for (int i = 0; i < s_cfg.max_clients; i++) {
        if (s_cli[i].fd < 0) {
            c = &s_cli[i];
            break;
        }
    }
    if (!c) {
        static const char busy[] = "opengr8on CLU: too many log clients\r\n";
        send(fd, busy, sizeof busy - 1, MSG_DONTWAIT);
        close(fd);
        return;
    }
    // A slow reader must not stall the server for long: drop it instead.
    struct timeval tv = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    c->fd = fd;
    c->iac = 0;
    strlcpy(c->ip, ip, sizeof c->ip);

    // Replay history, starting at a line boundary if the ring has wrapped.
    taskENTER_CRITICAL(&s_lock);
    uint64_t head = s_head, rd = head > s_size ? head - s_size : 0;
    if (head > s_size) {
        while (rd < head && s_buf[rd % s_size] != '\n') rd++;
        if (rd < head) rd++;
    }
    taskEXIT_CRITICAL(&s_lock);
    c->rd = rd;
    ESP_LOGI(TAG, "client %s connected", ip);
}

/** @brief Send everything new in the ring buffer to the client. */
static void client_flush(client_t *c)
{
    static uint8_t buf[CHUNK];
    for (;;) {
        uint64_t lost;
        size_t n = ring_get(&c->rd, buf, sizeof buf, &lost);
        if (lost) {
            char note[48];
            int k = snprintf(note, sizeof note, "\r\n[... %llu bytes dropped]\r\n",
                             (unsigned long long)lost);
            if (!send_all(c->fd, note, k)) {
                client_close(c, "send failed");
                return;
            }
        }
        if (!n) {
            return;
        }
        if (!send_all(c->fd, buf, n)) {
            client_close(c, "send failed");
            return;
        }
    }
}

/**
 * @brief Telnet input parser states (client_read).
 *
 * Input is ignored (read-only console), except Ctrl-C / Ctrl-D / telnet IP (IAC 244), which
 * close the session. Telnet option negotiation (`IAC WILL/WONT/DO/DONT <opt>`, `IAC SB ... IAC SE`)
 * is skipped so its option bytes (e.g. SGA = 3) are not taken for Ctrl-C.
 */
enum { IAC_NONE, IAC_CMD, IAC_OPT, IAC_SB, IAC_SB_IAC };

/** @brief Read and discard client input; close on Ctrl-C / Ctrl-D / IAC IP or EOF. */
static void client_read(client_t *c)
{
    uint8_t b[64];
    int n = recv(c->fd, b, sizeof b, MSG_DONTWAIT);
    if (n == 0) {
        client_close(c, "peer closed");
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            client_close(c, "recv error");
        }
        return;
    }
    char hex[3 * 16 + 4];
    int h = 0;
    for (int i = 0; i < n && i < 16; i++) h += snprintf(hex + h, sizeof hex - h, " %02x", b[i]);
    ESP_LOGI(TAG, "client %s sent %d bytes:%s%s", c->ip, n, hex, n > 16 ? " ..." : "");

    for (int i = 0; i < n; i++) {
        uint8_t ch = b[i];
        switch (c->iac) {
        case IAC_NONE:
            if (ch == 255) {
                c->iac = IAC_CMD;
            } else if (ch == 0x03 || ch == 0x04) {
                client_close(c, ch == 0x03 ? "Ctrl-C" : "Ctrl-D");
                return;
            }
            break;
        case IAC_CMD:
            if (ch == 244) {
                client_close(c, "telnet IP");
                return;
            }
            c->iac = ch >= 251 && ch <= 254 ? IAC_OPT : ch == 250 ? IAC_SB : IAC_NONE;
            break;
        case IAC_OPT: c->iac = IAC_NONE; break;
        case IAC_SB: if (ch == 255) c->iac = IAC_SB_IAC; break;
        case IAC_SB_IAC: c->iac = ch == 240 ? IAC_NONE : IAC_SB; break;
        }
    }
}

/**
 * @brief TCP server task: accept clients, read their input and stream the log every POLL_MS.
 */
static void server_task(void *arg)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(s_cfg.port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) {
        ESP_LOGE(TAG, "cannot listen on tcp/%u (errno %d)", s_cfg.port, errno);
        if (ls >= 0) close(ls);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "log console on tcp/%u", s_cfg.port);

    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        int maxfd = ls;
        for (int i = 0; i < s_cfg.max_clients; i++) {
            if (s_cli[i].fd >= 0) {
                FD_SET(s_cli[i].fd, &rf);
                if (s_cli[i].fd > maxfd) maxfd = s_cli[i].fd;
            }
        }
        struct timeval tv = {.tv_usec = POLL_MS * 1000};
        if (select(maxfd + 1, &rf, NULL, NULL, &tv) > 0) {
            if (FD_ISSET(ls, &rf)) {
                client_accept(ls);
            }
            for (int i = 0; i < s_cfg.max_clients; i++) {
                if (s_cli[i].fd >= 0 && FD_ISSET(s_cli[i].fd, &rf)) {
                    client_read(&s_cli[i]);
                }
            }
        }
        for (int i = 0; i < s_cfg.max_clients; i++) {
            if (s_cli[i].fd >= 0) {
                client_flush(&s_cli[i]);
            }
        }
    }
}

/**
 * @brief Parse the allow setting ("", "a.b.c.d" or "a.b.c.d/nn") into s_allow_ip / s_allow_mask.
 */
static void parse_allow(const char *s)
{
    s_allow_ip = s_allow_mask = 0;
    if (!s || !*s) {
        return;
    }
    char ip[16];
    int bits = 32;
    const char *slash = strchr(s, '/');
    size_t l = slash ? (size_t)(slash - s) : strlen(s);
    struct in_addr in;
    if (l >= sizeof ip || (slash && (bits = atoi(slash + 1)) < 0) || bits > 32) {
        goto bad;
    }
    memcpy(ip, s, l);
    ip[l] = 0;
    if (!inet_aton(ip, &in)) {
        goto bad;
    }
    s_allow_mask = bits ? htonl(0xffffffffu << (32 - bits)) : 0;
    s_allow_ip = in.s_addr & s_allow_mask;
    return;
bad:
    // Fail closed: a typo must not open the console to everyone.
    ESP_LOGE(TAG, "CLU_TELNET_ALLOW \"%s\" invalid, refusing all clients", s);
    s_allow_ip = 0xffffffffu;
    s_allow_mask = 0xffffffffu;
}

void clu_telnet_init(const clu_telnet_cfg_t *cfg)
{
    s_cfg = *cfg;
    if (s_cfg.max_clients < 1) s_cfg.max_clients = 1;
    s_size = s_cfg.history > 256 ? s_cfg.history : 256;
    s_buf = heap_caps_malloc(s_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_cli = calloc(s_cfg.max_clients, sizeof *s_cli);
    if (!s_buf || !s_cli) {
        ESP_LOGE(TAG, "no memory for %u byte log history", (unsigned)s_size);
        free(s_buf);
        free(s_cli);
        s_buf = NULL;
        s_cli = NULL;
        return;
    }
    for (int i = 0; i < s_cfg.max_clients; i++) s_cli[i].fd = -1;
    parse_allow(s_cfg.allow);
    s_orig = esp_log_set_vprintf(log_hook);
}

void clu_telnet_start(void)
{
    if (!s_buf) {
        return;
    }
    xTaskCreate(server_task, "telnet", 4096, NULL, 2, NULL);
}
