// Host stand-in for a CLU's secure channel: the clu_secure engine on a real UDP socket, answering a binary
// REQ with ACK + RESP `echo:<script>`. Used by
// test_sec_tool.sh to check tools/clu_sec.py against the C engine (transport framing, AAD, nonces).
//
//     sec_peer <port> <sn> <pairing 0|1> [seconds] [pairing window seconds]
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "clu_sec_engine.h"
#include "psa/crypto.h"

static int g_sock;
static bool g_pairing;
static uint32_t g_window_end; // 0 = pairing as g_pairing for the whole run
static sec_engine_t g_e;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void op_send(void *ctx, uint32_t ip, uint16_t port, const uint8_t *buf, size_t len)
{
    (void)ctx;
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip)};
    sendto(g_sock, buf, len, 0, (struct sockaddr *)&to, sizeof to);
}

static uint64_t op_wall_ms(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

// REQ(1) id(4 LE) script -> ACK(3) id, RESP(2) id status(1) "echo:<script>"
static void op_deliver(void *ctx, uint32_t handle, const sec_session_t *s, const uint8_t *plain, size_t len)
{
    (void)ctx;
    (void)s;
    if (len < 5 || plain[0] != 1) {
        return;
    }
    uint8_t out[1200] = {3, plain[1], plain[2], plain[3], plain[4]};
    sec_engine_send(&g_e, handle, out, 5, now_ms());
    size_t slen = len - 5 < 1000 ? len - 5 : 1000;
    out[0] = 2;
    out[5] = 0;
    memcpy(out + 6, "echo:", 5);
    memcpy(out + 11, plain + 5, slen);
    sec_engine_send(&g_e, handle, out, 11 + slen, now_ms());
}

static bool op_allowed(void *ctx)
{
    (void)ctx;
    return g_pairing && (!g_window_end || (int32_t)(g_window_end - now_ms()) > 0);
}

static void op_random(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    psa_generate_random(out, len);
}

int main(int argc, char **argv)
{
    if (argc < 4 || psa_crypto_init() != PSA_SUCCESS) {
        fprintf(stderr, "usage: sec_peer <port> <sn> <pairing 0|1> [seconds] [window seconds]\n");
        return 2;
    }
    g_pairing = atoi(argv[3]) != 0;
    int seconds = argc > 4 ? atoi(argv[4]) : 30;
    if (argc > 5) {
        g_window_end = now_ms() + (uint32_t)atoi(argv[5]) * 1000u;
    }
    noise_keypair_t kp;
    noise_keypair_generate(&kp);
    sec_ops_t ops = {op_send, op_deliver, NULL, op_allowed, NULL, op_random, NULL, op_wall_ms};
    sec_engine_init(&g_e, &kp, (uint32_t)strtoul(argv[2], NULL, 10), &ops);

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(g_sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    printf("ready\n");
    fflush(stdout);
    uint32_t end = now_ms() + (uint32_t)seconds * 1000u;
    while ((int32_t)(end - now_ms()) > 0) {
        uint8_t buf[1600];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(g_sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n > 0) {
            sec_engine_input(&g_e, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), buf, (size_t)n, now_ms());
        }
        sec_engine_tick(&g_e, now_ms());
    }
    return 0;
}
