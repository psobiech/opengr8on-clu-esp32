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
static void ring_put(const char *line_text, size_t text_length)
{
    taskENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < text_length; i++) {
        if (line_text[i] == '\n' && (i == 0 || line_text[i - 1] != '\r')) {
            s_buf[s_head++ % s_size] = '\r'; // telnet wants CRLF
        }
        s_buf[s_head++ % s_size] = (uint8_t)line_text[i];
    }
    taskEXIT_CRITICAL(&s_lock);
}

/**
 * @brief vprintf hook: print as before (UART) and copy the formatted line into the ring buffer.
 */
static int log_hook(const char *fmt, va_list arguments)
{
    va_list ap2;
    va_copy(ap2, arguments);
    int printed_count = s_orig ? s_orig(fmt, arguments) : vprintf(fmt, arguments);
    char line[LOG_LINE_MAX];
    int formatted_length = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);
    if (formatted_length > 0) {
        if (formatted_length >= (int)sizeof line) {
            memcpy(line + sizeof line - 5, "...\n", 4);
            formatted_length = sizeof line - 1;
        }
        ring_put(line, formatted_length);
    }
    return printed_count;
}

/**
 * @brief Copy up to cap bytes from absolute position *rd.
 *
 * @return Bytes copied; *lost = bytes the reader fell behind (overwritten before it got them),
 *         *rd is advanced past them.
 */
static size_t ring_get(uint64_t *read_position, uint8_t *dst, size_t cap, uint64_t *lost)
{
    taskENTER_CRITICAL(&s_lock);
    uint64_t head = s_head;
    uint64_t oldest = head > s_size ? head - s_size : 0;
    *lost = 0;
    if (*read_position < oldest) {
        *lost = oldest - *read_position;
        *read_position = oldest;
    }
    size_t bytes_copied = head - *read_position < cap ? (size_t)(head - *read_position) : cap;
    for (size_t i = 0; i < bytes_copied; i++) {
        dst[i] = s_buf[(*read_position + i) % s_size];
    }
    *read_position += bytes_copied;
    taskEXIT_CRITICAL(&s_lock);
    return bytes_copied;
}

// ---- TCP server ----------------------------------------------------------------------------

/** @brief Send the whole buffer; false on socket error. */
static bool send_all(int socket_fd, const void *send_buffer, size_t remaining_bytes)
{
    const uint8_t *buffer_bytes = send_buffer;
    while (remaining_bytes) {
        int bytes_sent = send(socket_fd, buffer_bytes, remaining_bytes, 0);
        if (bytes_sent <= 0) {
            return false;
        }
        buffer_bytes += bytes_sent;
        remaining_bytes -= bytes_sent;
    }
    return true;
}

/** @brief Close a client session (logged with @p why). */
static void client_close(client_t *client, const char *why)
{
    ESP_LOGI(TAG, "client %s closed: %s", client->ip, why);
    close(client->fd);
    client->fd = -1;
}

/**
 * @brief Accept a connection on @p ls: check the allow list and free slots, start from the buffered history.
 */
static void client_accept(int listen_socket)
{
    struct sockaddr_in peer_address;
    socklen_t peer_address_length = sizeof peer_address;
    int client_socket = accept(listen_socket, (struct sockaddr *)&peer_address, &peer_address_length);
    if (client_socket < 0) {
        return;
    }
    char ip[16];
    inet_ntoa_r(peer_address.sin_addr, ip, sizeof ip);
    if ((peer_address.sin_addr.s_addr & s_allow_mask) != (s_allow_ip & s_allow_mask)) {
        ESP_LOGW(TAG, "refused %s (CLU_TELNET_ALLOW)", ip);
        close(client_socket);
        return;
    }
    client_t *available_client = NULL;
    for (int i = 0; i < s_cfg.max_clients; i++) {
        if (s_cli[i].fd < 0) {
            available_client = &s_cli[i];
            break;
        }
    }
    if (!available_client) {
        static const char busy[] = "opengr8on CLU: too many log clients\r\n";
        send(client_socket, busy, sizeof busy - 1, MSG_DONTWAIT);
        close(client_socket);
        return;
    }
    // A slow reader must not stall the server for long: drop it instead.
    struct timeval send_timeout = {.tv_sec = 2};
    setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof send_timeout);
    int one = 1;
    setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    available_client->fd = client_socket;
    available_client->iac = 0;
    strlcpy(available_client->ip, ip, sizeof available_client->ip);

    // Replay history, starting at a line boundary if the ring has wrapped.
    taskENTER_CRITICAL(&s_lock);
    uint64_t head = s_head, read_position = head > s_size ? head - s_size : 0;
    if (head > s_size) {
        while (read_position < head && s_buf[read_position % s_size] != '\n') read_position++;
        if (read_position < head) read_position++;
    }
    taskEXIT_CRITICAL(&s_lock);
    available_client->rd = read_position;
    ESP_LOGI(TAG, "client %s connected", ip);
}

/** @brief Send everything new in the ring buffer to the client. */
static void client_flush(client_t *client)
{
    static uint8_t buf[CHUNK];
    for (;;) {
        uint64_t lost;
        size_t bytes_copied = ring_get(&client->rd, buf, sizeof buf, &lost);
        if (lost) {
            char note[48];
            int notice_length = snprintf(note, sizeof note, "\r\n[... %llu bytes dropped]\r\n",
                             (unsigned long long)lost);
            if (!send_all(client->fd, note, notice_length)) {
                client_close(client, "send failed");
                return;
            }
        }
        if (!bytes_copied) {
            return;
        }
        if (!send_all(client->fd, buf, bytes_copied)) {
            client_close(client, "send failed");
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
static void client_read(client_t *client)
{
    uint8_t input_buffer[64];
    int bytes_received = recv(client->fd, input_buffer, sizeof input_buffer, MSG_DONTWAIT);
    if (bytes_received == 0) {
        client_close(client, "peer closed");
        return;
    }
    if (bytes_received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            client_close(client, "recv error");
        }
        return;
    }
    char hex[3 * 16 + 4];
    int hex_length = 0;
    for (int i = 0; i < bytes_received && i < 16; i++) hex_length += snprintf(hex + hex_length, sizeof hex - hex_length, " %02x", input_buffer[i]);
    ESP_LOGI(TAG, "client %s sent %d bytes:%s%s", client->ip, bytes_received, hex, bytes_received > 16 ? " ..." : "");

    for (int i = 0; i < bytes_received; i++) {
        uint8_t input_character = input_buffer[i];
        switch (client->iac) {
        case IAC_NONE:
            if (input_character == 255) {
                client->iac = IAC_CMD;
            } else if (input_character == 0x03 || input_character == 0x04) {
                client_close(client, input_character == 0x03 ? "Ctrl-C" : "Ctrl-D");
                return;
            }
            break;
        case IAC_CMD:
            if (input_character == 244) {
                client_close(client, "telnet IP");
                return;
            }
            client->iac = input_character >= 251 && input_character <= 254 ? IAC_OPT : input_character == 250 ? IAC_SB : IAC_NONE;
            break;
        case IAC_OPT: client->iac = IAC_NONE; break;
        case IAC_SB: if (input_character == 255) client->iac = IAC_SB_IAC; break;
        case IAC_SB_IAC: client->iac = input_character == 240 ? IAC_NONE : IAC_SB; break;
        }
    }
}

/**
 * @brief TCP server task: accept clients, read their input and stream the log every POLL_MS.
 */
static void server_task(void *arg)
{
    int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in listen_address = {
        .sin_family = AF_INET,
        .sin_port = htons(s_cfg.port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (listen_socket < 0 || bind(listen_socket, (struct sockaddr *)&listen_address, sizeof listen_address) < 0 || listen(listen_socket, 1) < 0) {
        ESP_LOGE(TAG, "cannot listen on tcp/%u (errno %d)", s_cfg.port, errno);
        if (listen_socket >= 0) close(listen_socket);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "log console on tcp/%u", s_cfg.port);

    for (;;) {
        fd_set readable_fds;
        FD_ZERO(&readable_fds);
        FD_SET(listen_socket, &readable_fds);
        int maxfd = listen_socket;
        for (int i = 0; i < s_cfg.max_clients; i++) {
            if (s_cli[i].fd >= 0) {
                FD_SET(s_cli[i].fd, &readable_fds);
                if (s_cli[i].fd > maxfd) maxfd = s_cli[i].fd;
            }
        }
        struct timeval poll_timeout = {.tv_usec = POLL_MS * 1000};
        if (select(maxfd + 1, &readable_fds, NULL, NULL, &poll_timeout) > 0) {
            if (FD_ISSET(listen_socket, &readable_fds)) {
                client_accept(listen_socket);
            }
            for (int i = 0; i < s_cfg.max_clients; i++) {
                if (s_cli[i].fd >= 0 && FD_ISSET(s_cli[i].fd, &readable_fds)) {
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
static void parse_allow(const char *allow_setting)
{
    s_allow_ip = s_allow_mask = 0;
    if (!allow_setting || !*allow_setting) {
        return;
    }
    char ip[16];
    int bits = 32;
    const char *slash = strchr(allow_setting, '/');
    size_t address_length = slash ? (size_t)(slash - allow_setting) : strlen(allow_setting);
    struct in_addr in;
    if (address_length >= sizeof ip || (slash && (bits = atoi(slash + 1)) < 0) || bits > 32) {
        goto bad;
    }
    memcpy(ip, allow_setting, address_length);
    ip[address_length] = 0;
    if (!inet_aton(ip, &in)) {
        goto bad;
    }
    s_allow_mask = bits ? htonl(0xffffffffu << (32 - bits)) : 0;
    s_allow_ip = in.s_addr & s_allow_mask;
    return;
bad:
    // Fail closed: a typo must not open the console to everyone.
    ESP_LOGE(TAG, "CLU_TELNET_ALLOW \"%s\" invalid, refusing all clients", allow_setting);
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
