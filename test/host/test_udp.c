// Host tests for the CLU UDP command server. udp_server.c is included directly (its handlers and
// tasks are static) and run against fake sockets, a fake cipher and stubs of the modules it calls.
//
// The tasks are endless loops: a task "blocks forever" (empty queue, no more scripted datagrams,
// vTaskDelete) by longjmp-ing back to run_task(), so each test feeds a script and runs the task
// until it has consumed it.
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../main/udp_server.c"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

static void check_str(const char *name, const char *got, const char *want)
{
    int ok = got && strcmp(got, want) == 0;
    check(name, ok);
    if (!ok) {
        printf("  got  %s\n  want %s\n", got ? got : "(null)", want);
    }
}

// ---------------------------------------------------------------- task escape
static jmp_buf s_escape;
static bool s_in_task;

static void block_forever(void)
{
    if (!s_in_task) {
        fprintf(stderr, "a task would block outside run_task()\n");
        abort();
    }
    longjmp(s_escape, 1);
}

static void run_task(void (*fn)(void *))
{
    s_in_task = true;
    if (setjmp(s_escape) == 0) {
        fn(NULL);
    }
    s_in_task = false;
}

// ---------------------------------------------------------------- FreeRTOS stubs
struct host_queue {
    size_t item, cap, head, n;
    uint8_t *buf;
};
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item)
{
    QueueHandle_t q = calloc(1, sizeof *q);
    q->item = item;
    q->cap = len;
    q->buf = calloc(len, item);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    if (q->n == q->cap) {
        return pdFALSE;
    }
    memcpy(q->buf + ((q->head + q->n) % q->cap) * q->item, item, q->item);
    q->n++;
    return pdTRUE;
}
// Blocking receives: the k-th one (counted by s_nwait) first waits s_wait[k] ticks for its item.
static TickType_t s_wait[8];
static int s_nwait;
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    if (!q->n) {
        if (wait == portMAX_DELAY) {
            block_forever();
        }
        return pdFALSE;
    }
    if (wait == portMAX_DELAY && s_nwait < 8) {
        host_ticks += s_wait[s_nwait++];
    }
    memcpy(item, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->cap;
    q->n--;
    return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t q)
{
    q->head = q->n = 0;
    return pdTRUE;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
static TickType_t s_delayed;
void vTaskDelay(TickType_t t)
{
    s_delayed += t;
    host_ticks += t;
}
void vTaskDelete(TaskHandle_t t)
{
    (void)t;
    block_forever();
}
uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t w) { (void)c, (void)w; return 0; }
void xTaskNotifyGive(TaskHandle_t t) { (void)t; }
static const char *s_tasks[8];
static int s_ntasks;
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *n, uint32_t s, void *a, UBaseType_t p,
                                   TaskHandle_t *h, BaseType_t c)
{
    (void)fn, (void)s, (void)a, (void)p, (void)h, (void)c;
    s_tasks[s_ntasks++] = n; // never run here: the test runs the task functions itself
    return pdTRUE;
}

// ---------------------------------------------------------------- fake cipher
// Length-preserving like AES-CBC + PKCS#7 (30 -> 32 bytes etc.), keyed by key AND IV, and a wrong
// key yields garbage that fails the padding check: enough for key selection and reply checks.
static uint8_t keystream(const clu_cipher_t *c, size_t i)
{
    uint32_t h = 2166136261u;
    for (int j = 0; j < 16; j++) {
        h = (h ^ c->key[j]) * 16777619u;
    }
    for (int j = 0; j < 16; j++) {
        h = (h ^ c->iv[j]) * 16777619u;
    }
    h = (h ^ (uint32_t)i) * 16777619u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (uint8_t)h;
}
int clu_encrypt(const clu_cipher_t *c, const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    if (len > 1500 || cap < len + 16) {
        return -1;
    }
    uint8_t tmp[1500 + 16];
    memcpy(tmp, in, len);
    size_t n = clu_pkcs7_pad(tmp, len);
    for (size_t i = 0; i < n; i++) {
        out[i] = tmp[i] ^ keystream(c, i);
    }
    return (int)n;
}
int clu_decrypt(const clu_cipher_t *c, const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    if (len == 0 || len % 16 || cap < len) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = in[i] ^ keystream(c, i);
    }
    return clu_pkcs7_unpad(out, len);
}
static uint8_t s_rnd_seed = 0x40;
void clu_random(uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        out[i] = s_rnd_seed++;
    }
}

// ---------------------------------------------------------------- module stubs
clu_state_t g_clu;
static int s_saved_keys;
void clu_state_save_keys(void) { s_saved_keys++; }

static bool s_net_ok = true;
static char s_net_ip[16], s_net_gw[16];
static int s_net_applied;
bool clu_net_set_static(const char *ip, const char *gw)
{
    strcpy(s_net_ip, ip);
    strcpy(s_net_gw, gw);
    return s_net_ok;
}
void clu_net_apply(void) { s_net_applied++; }

// Time since boot (setup window): 0 = just booted, window open.
static int64_t s_now_us;
int64_t esp_timer_get_time(void) { return s_now_us; }

static uint64_t s_gpio_mask;
esp_err_t gpio_config(const gpio_config_t *c)
{
    s_gpio_mask = c->pin_bit_mask;
    return c->mode == GPIO_MODE_INPUT && c->pull_up_en == GPIO_PULLUP_ENABLE ? ESP_OK : ESP_FAIL;
}
int gpio_get_level(gpio_num_t gpio) { (void)gpio; return 1; }

static int s_tftp_on = -1;
static uint32_t s_tftp_peer;
void clu_tftp_enable(bool on, uint32_t peer)
{
    s_tftp_on = on;
    s_tftp_peer = peer;
}

static int s_diag_err;
static char s_diag_ip[16];
int clu_diag_write(const char *ip)
{
    strcpy(s_diag_ip, ip);
    return s_diag_err;
}

static bool s_refresh_ok = true;
static int s_refreshes;
static uint32_t s_refresh_timeout;
bool tfbus_master_refresh(uint32_t timeout_ms)
{
    s_refreshes++;
    s_refresh_timeout = timeout_ms;
    return s_refresh_ok;
}

static esp_err_t s_lua_rc = ESP_OK;
static char s_lua_val[64] = "42";
static char s_lua_script[256], s_lua_from[16];
static int s_lua_calls;
esp_err_t clu_lua_call(const char *expr, const char *from_ip, char *out, size_t cap, int timeout_ms)
{
    (void)timeout_ms;
    s_lua_calls++;
    snprintf(s_lua_script, sizeof s_lua_script, "%s", expr);
    snprintf(s_lua_from, sizeof s_lua_from, "%s", from_ip);
    if (s_lua_rc == ESP_OK) {
        snprintf(out, cap, "%s", s_lua_val);
    }
    return s_lua_rc;
}
static bool s_reload_ok = true;
static int s_reloads, s_halts, s_restarts;
bool clu_lua_reload(int timeout_ms) { (void)timeout_ms; s_reloads++; return s_reload_ok; }
bool clu_lua_halt(int timeout_ms) { (void)timeout_ms; s_halts++; return true; }
static clu_lua_state_t s_lua_state = CLU_LUA_OK;
clu_lua_state_t clu_lua_state(void) { return s_lua_state; }
void esp_restart(void) { s_restarts++; }

static char s_posted[32][64];
static uint32_t s_posted_id[32];
static int s_nposted;
void clu_lua_post_remote(uint32_t id, const char *result)
{
    s_posted_id[s_nposted] = id;
    snprintf(s_posted[s_nposted], sizeof s_posted[0], "%s", result ? result : "(nil)");
    s_nposted++;
}

// ---------------------------------------------------------------- fake sockets
#define FD_SERVER 3
static bool s_sock_fail, s_bind_fail, s_send_fail;
static int s_closed;
static int s_next_fd = FD_SERVER;
int host_socket(int domain, int type, int proto)
{
    (void)domain, (void)type, (void)proto;
    return s_sock_fail ? -1 : s_next_fd++;
}
int host_bind(int s, const struct sockaddr *a, socklen_t len) { (void)s, (void)a, (void)len; return s_bind_fail ? -1 : 0; }
int host_setsockopt(int s, int level, int opt, const void *v, socklen_t len)
{
    (void)s, (void)level, (void)opt, (void)v, (void)len;
    return 0;
}
int host_close(int s) { (void)s; s_closed++; return 0; }

// Sent datagrams (raw ciphertext + destination).
typedef struct {
    uint8_t data[BUF_SZ + 16];
    size_t len;
    struct sockaddr_in to;
} dgram_t;
static dgram_t s_sent[32];
static int s_nsent;
ssize_t host_sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tl)
{
    (void)s, (void)flags, (void)tl;
    if (s_send_fail) {
        errno = ENETUNREACH;
        return -1;
    }
    dgram_t *d = &s_sent[s_nsent++ % 32];
    memcpy(d->data, buf, len);
    d->len = len;
    memcpy(&d->to, to, sizeof d->to);
    return (ssize_t)len;
}

// Scripted receive: datagrams, errors, or a reply built from the last sent request.
enum { RX_DATA, RX_ERR, RX_EAGAIN, RX_REPLY };
typedef struct {
    int kind;
    uint8_t data[BUF_SZ];
    size_t len;
    struct sockaddr_in from;
    char text[128]; // RX_REPLY: "%s" gets the session of the last sent req
} rx_t;
static rx_t s_rx[32];
static int s_nrx, s_rxpos;
// No script left: the UDP task blocks forever; the remote task's SO_RCVTIMEO slice expires.
static bool s_rx_timeout_slices;

static struct sockaddr_in addr(const char *ip, int port)
{
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_aton(ip, &a.sin_addr);
    return a;
}

static void rx_reset(void)
{
    s_nrx = s_rxpos = 0;
}

static void rx_add(const clu_cipher_t *key, const void *plain, size_t len, const char *from_ip)
{
    rx_t *r = &s_rx[s_nrx++];
    memset(r, 0, sizeof *r);
    r->kind = RX_DATA;
    r->len = (size_t)clu_encrypt(key, plain, len, r->data, sizeof r->data);
    r->from = addr(from_ip, 1234);
}

static void rx_add_str(const clu_cipher_t *key, const char *s, const char *from_ip)
{
    rx_add(key, s, strlen(s), from_ip);
}

static void rx_add_raw(const uint8_t *data, size_t len, const char *from_ip)
{
    rx_t *r = &s_rx[s_nrx++];
    memset(r, 0, sizeof *r);
    r->kind = RX_DATA;
    memcpy(r->data, data, len);
    r->len = len;
    r->from = addr(from_ip, 1234);
}

static void rx_add_kind(int kind)
{
    rx_t *r = &s_rx[s_nrx++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
}

// Reply to the last sent request: fmt gets the request's session (8 hex chars) as its %s.
static void rx_add_reply(const char *from_ip, const char *fmt)
{
    rx_t *r = &s_rx[s_nrx++];
    memset(r, 0, sizeof *r);
    r->kind = RX_REPLY;
    r->from = addr(from_ip, 1234);
    snprintf(r->text, sizeof r->text, "%s", fmt);
}

static int sent_plain(int i, const clu_cipher_t *key, char *out, size_t cap);

ssize_t host_recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fl)
{
    (void)s, (void)flags;
    if (s_rxpos >= s_nrx) {
        if (s_rx_timeout_slices) {
            host_ticks += 200;
            errno = EAGAIN;
            return -1;
        }
        block_forever();
    }
    rx_t *r = &s_rx[s_rxpos++];
    switch (r->kind) {
    case RX_ERR:
        errno = EBADF;
        return -1;
    case RX_EAGAIN:
        host_ticks += 200;
        errno = EAGAIN;
        return -1;
    case RX_REPLY: {
        char req[BUF_SZ + 1], text[256], sess[9] = "";
        if (sent_plain(s_nsent - 1, &g_clu.project, req, sizeof req) > 0) {
            // req:<ip>:<session>:<script>
            const char *p = strchr(req + 4, ':');
            if (p) {
                snprintf(sess, sizeof sess, "%.8s", p + 1);
            }
        }
        snprintf(text, sizeof text, r->text, sess);
        r->len = (size_t)clu_encrypt(&g_clu.project, (const uint8_t *)text, strlen(text), r->data,
                                     sizeof r->data);
        break;
    }
    default:
        break;
    }
    size_t n = r->len < len ? r->len : len;
    memcpy(buf, r->data, n);
    memcpy(from, &r->from, sizeof r->from);
    *fl = sizeof r->from;
    return (ssize_t)n;
}

// ---------------------------------------------------------------- helpers
static void sent_reset(void) { s_nsent = 0; }

// Decrypt sent datagram i with key; NUL-terminated text in out. Returns its length or -1.
static int sent_plain(int i, const clu_cipher_t *key, char *out, size_t cap)
{
    if (i < 0 || i >= s_nsent) {
        return -1;
    }
    int n = clu_decrypt(key, s_sent[i].data, s_sent[i].len, (uint8_t *)out, cap - 1);
    if (n >= 0) {
        out[n] = 0;
    }
    return n;
}

static const char *sent_str(int i, const clu_cipher_t *key)
{
    static char buf[BUF_SZ + 1];
    return sent_plain(i, key, buf, sizeof buf) >= 0 ? buf : NULL;
}

static clu_cipher_t pin_key(void)
{
    clu_cipher_t k;
    clu_default_key((const uint8_t *)g_clu.pin, k.key);
    memcpy(k.iv, g_clu.temp.iv, 16);
    return k;
}

static clu_cipher_t bcast_with_iv(const uint8_t iv[16])
{
    clu_cipher_t k = *broadcast_key();
    memcpy(k.iv, iv, 16);
    return k;
}

// req_discovery_clu packet: enc(rnd under `under`) ':' iv ':' "req_discovery_clu:" ...
static size_t discovery_req(uint8_t *out, const clu_cipher_t *under, const uint8_t rnd[30], const uint8_t iv[16])
{
    clu_encrypt(under, rnd, 30, out, 48);
    out[32] = ':';
    memcpy(out + 33, iv, 16);
    out[49] = ':';
    memcpy(out + 50, "req_discovery_clu:", 18);
    return 68;
}

// req_set_key packet: rnd(32) ':' new_iv(16) ':' "req_set_key:" new_key(16)
static size_t set_key_req(uint8_t *out, const uint8_t new_key[16], size_t keylen, const uint8_t new_iv[16])
{
    memset(out, 0x11, 32);
    out[32] = ':';
    memcpy(out + 33, new_iv, 16);
    out[49] = ':';
    memcpy(out + 50, "req_set_key:", 12);
    memcpy(out + 62, new_key, keylen);
    return 62 + keylen;
}

// Dispatch a plaintext directly, as udp_task would after decrypting it with `used`.
static void dispatch_str(const char *s, const clu_cipher_t *used, bool project, bool temp, bool bcast,
                         const char *from_ip)
{
    static uint8_t buf[BUF_SZ + 1];
    size_t n = strlen(s);
    memcpy(buf, s, n);
    struct sockaddr_in from = addr(from_ip, 1234);
    dispatch(buf, n, used, project, temp, bcast, &from);
}

// ---------------------------------------------------------------- tests
static void test_before_start(void)
{
    udp_server_send_report("192.168.1.9", 5000, "x"); // no socket yet
    send_str(&g_clu.project, "resp:OK", &(struct sockaddr_in){0}); // no tx lock yet
    check("nothing sent before udp_server_start()", s_nsent == 0);
    check("remote exec before start -> 0", udp_server_remote_exec(0x0a000001, "x") == 0);
    s_now_us = 3600LL * 1000000; // e.g. a long wait for WiFi
    check("setup window: open (not counting) before udp_server_start()", setup_window_open());
    s_now_us = 0;
}

static void test_start_and_socket_errors(void)
{
    udp_server_start();
    check("start: discovery, udp and remote tasks created",
          s_ntasks == 4 && strcmp(s_tasks[0], "clu_disc") == 0 && strcmp(s_tasks[1], "clu_udp") == 0 &&
              strcmp(s_tasks[2], "clu_remote") == 0 && strcmp(s_tasks[3], "clu_setup") == 0);
    check("setup button: GPIO0 configured as input with pull-up", s_gpio_mask == 1);

    s_sock_fail = true;
    run_task(udp_task);
    check("udp task: socket() failure -> task deleted, no socket", s_sock < 0);
    s_sock_fail = false;

    s_bind_fail = true;
    int closed = s_closed;
    run_task(udp_task);
    check("udp task: bind() failure -> socket closed, task deleted", s_sock < 0 && s_closed == closed + 1);
    s_bind_fail = false;
}

static void test_discovery(void)
{
    static const uint8_t om_iv[16] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
                                      0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf};
    uint8_t rnd[30], pkt[128];
    for (int i = 0; i < 30; i++) {
        rnd[i] = (uint8_t)(0x30 + i);
    }
    size_t n = discovery_req(pkt, &g_clu.project, rnd, om_iv);
    rx_reset();
    rx_add(broadcast_key(), pkt, n, "192.168.1.50");
    rx_add_kind(RX_ERR); // recvfrom error: ignored
    run_task(udp_task);
    check("discovery: queued for the discovery task, nothing sent from the UDP task",
          s_discovery_q->n == 1 && s_nsent == 0 && g_clu.has_temp);

    s_refreshes = 0;
    sent_reset();
    s_delayed = 0;
    run_task(discovery_task);
    check("discovery task: bus rescanned once (20 s timeout), 3 replies 0/50/100 ms apart",
          s_refreshes == 1 && s_refresh_timeout == 20000 && s_nsent == 3 && s_delayed == 150);

    clu_cipher_t reply_key = bcast_with_iv(om_iv);
    char got[256];
    int pl = sent_plain(0, &reply_key, got, sizeof got);
    char want_tail[64];
    snprintf(want_tail, sizeof want_tail, ":resp_discovery_clu:%08lx:%s", (unsigned long)g_clu.sn, g_clu.mac);
    check("discovery reply under the broadcast key with OM's IV, sent to the requester",
          pl == 49 + (int)strlen(want_tail) && got[32] == ':' && memcmp(got + 33, g_clu.temp.iv, 16) == 0 &&
              strcmp(got + 49, want_tail) == 0 && s_sent[0].to.sin_addr.s_addr == inet_addr("192.168.1.50"));
    // enc(hash) under the temp key = default_key(PIN) + our temp IV: proves we know the project key
    uint8_t h[30], dh[32], want_h[30];
    clu_cipher_t tk = pin_key();
    clu_discovery_hash(rnd, 30, want_h);
    int hl = clu_decrypt(&tk, (uint8_t *)got, 32, dh, sizeof dh);
    memcpy(h, dh, 30);
    check("discovery: enc(hash(random)) under default_key(PIN) + temp IV",
          hl == 30 && memcmp(h, want_h, 30) == 0 && memcmp(g_clu.temp.key, tk.key, 16) == 0);
    check("discovery: the 3 copies are identical",
          s_sent[1].len == s_sent[0].len && memcmp(s_sent[1].data, s_sent[0].data, s_sent[0].len) == 0 &&
              memcmp(s_sent[2].data, s_sent[0].data, s_sent[0].len) == 0);

    // One task run, 3 requests: the 2nd 1 s after the 1st (no rescan), the 3rd 31 s later
    // (rescan). The rescans fail (timeout): the requests are answered anyway.
    uint8_t iv_before[16];
    memcpy(iv_before, g_clu.temp.iv, 16);
    rx_reset();
    for (int i = 0; i < 3; i++) {
        rx_add(broadcast_key(), pkt, n, "192.168.1.50");
    }
    run_task(udp_task);
    s_refresh_ok = false;
    s_refreshes = 0;
    s_wait[1] = 1000;
    s_wait[2] = 31000;
    s_nwait = 0;
    sent_reset();
    run_task(discovery_task);
    check("discovery within 30 s of a scan: no rescan; after 30 s: rescan; failed rescans still answered",
          s_refreshes == 2 && s_nsent == 9);
    check("discovery: temp IV kept across requests", memcmp(iv_before, g_clu.temp.iv, 16) == 0);
    s_refresh_ok = true;
    memset(s_wait, 0, sizeof s_wait);

    // OM with another project key: random does not decrypt -> answer with a random hash anyway
    clu_cipher_t other = {.key = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9}};
    n = discovery_req(pkt, &other, rnd, om_iv);
    rx_reset();
    rx_add(broadcast_key(), pkt, n, "192.168.1.51");
    run_task(udp_task);
    sent_reset();
    run_task(discovery_task);
    check("discovery under a foreign project key: still answered (OM then asks for the PIN)",
          s_nsent == 3 && sent_plain(0, &reply_key, got, sizeof got) > 49 && strstr(got + 49, "resp_discovery_clu"));

    // Queue full: further requests dropped until the discovery task catches up
    rx_reset();
    for (int i = 0; i < DISCOVERY_QUEUE + 2; i++) {
        rx_add(broadcast_key(), pkt, n, "192.168.1.52");
    }
    run_task(udp_task);
    check("discovery queue full: excess requests dropped", s_discovery_q->n == DISCOVERY_QUEUE);
    xQueueReset(s_discovery_q);
}

static void test_set_ip(void)
{
    char req[96];
    clu_cipher_t pk = pin_key();
    snprintf(req, sizeof req, "req_set_clu_ip:%08lx:192.168.1.77:192.168.1.1", (unsigned long)g_clu.sn);

    // broadcast, wrong SN: silently ignored (another CLU's request)
    rx_reset();
    rx_add_str(broadcast_key(), "req_set_clu_ip:deadbeef:192.168.1.77:192.168.1.1", "192.168.1.50");
    rx_add_str(broadcast_key(), "req_set_clu_ip", "192.168.1.50"); // no ':' -> not this command
    rx_add_str(broadcast_key(), "req:1.2.3.4:00000001:checkAlive()", "192.168.1.50"); // broadcast: only discovery/set_ip
    sent_reset();
    run_task(udp_task);
    check("broadcast: set_ip for another SN and other commands ignored", s_nsent == 0 && s_lua_calls == 0);

    // unicast (PIN key), wrong SN / malformed -> resp:ERROR
    rx_reset();
    rx_add_str(&pk, "req_set_clu_ip:deadbeef:192.168.1.77:192.168.1.1", "192.168.1.50");
    rx_add_str(&pk, "req_set_clu_ip:zz", "192.168.1.50");
    run_task(udp_task);
    check_str("set_ip, wrong SN under the PIN key -> resp:ERROR", sent_str(0, &pk), "resp:ERROR");
    check_str("set_ip, malformed -> resp:ERROR", sent_str(1, &pk), "resp:ERROR");

    // same IP as now: confirmed, nothing applied
    sent_reset();
    char same[96];
    snprintf(same, sizeof same, "req_set_clu_ip:%08lx:192.168.1.20:192.168.1.1", (unsigned long)g_clu.sn);
    rx_reset();
    rx_add_str(broadcast_key(), same, "192.168.1.50");
    run_task(udp_task);
    char want[64];
    snprintf(want, sizeof want, "resp_set_clu_ip:%08lx:192.168.1.20", (unsigned long)g_clu.sn);
    check_str("set_ip to the current IP: confirmed under the used key", sent_str(0, broadcast_key()), want);
    check("set_ip to the current IP: not applied", s_net_applied == 0 && !s_net_ip[0]);

    // new IP, saved: answer from the old address, then apply
    sent_reset();
    rx_reset();
    rx_add_str(&pk, req, "192.168.1.50");
    run_task(udp_task);
    snprintf(want, sizeof want, "resp_set_clu_ip:%08lx:192.168.1.77", (unsigned long)g_clu.sn);
    check_str("set_ip: new IP confirmed", sent_str(0, &pk), want);
    check("set_ip: static IP + gateway stored and applied after the reply",
          strcmp(s_net_ip, "192.168.1.77") == 0 && strcmp(s_net_gw, "192.168.1.1") == 0 && s_net_applied == 1);

    // storing fails
    s_net_ok = false;
    sent_reset();
    rx_reset();
    rx_add_str(&pk, req, "192.168.1.50");
    run_task(udp_task);
    check_str("set_ip: store failure -> resp:ERROR", sent_str(0, &pk), "resp:ERROR");
    check("set_ip: store failure -> not applied", s_net_applied == 1);
    s_net_ok = true;
}

static void test_set_key(void)
{
    static const uint8_t new_key[16] = {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
                                        0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f};
    static const uint8_t new_iv[16] = {0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87,
                                       0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f};
    clu_cipher_t pk = pin_key();
    uint8_t pkt[96];

    size_t n = set_key_req(pkt, new_key, 8, new_iv); // key truncated
    rx_reset();
    rx_add(&pk, pkt, n, "192.168.1.50");
    sent_reset();
    run_task(udp_task);
    check_str("set_key with a short key -> resp:ERROR under the used key", sent_str(0, &pk), "resp:ERROR");
    check("set_key short: key unchanged", s_saved_keys == 0 && memcmp(g_clu.project.key, new_key, 16) != 0);

    n = set_key_req(pkt, new_key, 16, new_iv);
    rx_reset();
    rx_add(&pk, pkt, n, "192.168.1.50");
    sent_reset();
    run_task(udp_task);
    check("set_key: new project key + IV stored, temp key dropped",
          s_saved_keys == 1 && memcmp(g_clu.project.key, new_key, 16) == 0 &&
              memcmp(g_clu.project.iv, new_iv, 16) == 0 && !g_clu.has_temp);
    check_str("set_key: resp:OK under the NEW key", sent_str(0, &g_clu.project), "resp:OK");
}

static void test_lua_requests(void)
{
    clu_cipher_t *prj = &g_clu.project;
    sent_reset();
    rx_reset();
    rx_add_str(prj, "req:192.168.1.20:00abcdef:SYSTEM:get(1)\r\n", "192.168.1.50");
    run_task(udp_task);
    check("req: script (CR/LF stripped) evaluated with the caller's IP",
          strcmp(s_lua_script, "SYSTEM:get(1)") == 0 && strcmp(s_lua_from, "192.168.1.50") == 0);
    check_str("req: resp:<our ip>:<session>:<value>", sent_str(0, prj), "resp:192.168.1.20:00abcdef:42");

    s_lua_rc = ESP_ERR_TIMEOUT;
    sent_reset();
    rx_reset();
    rx_add_str(prj, "req:192.168.1.20:00000002:slow()", "192.168.1.50");
    rx_add_str(prj, "req:192.168.1.20:00000003:checkAlive()", "192.168.1.50");
    run_task(udp_task);
    check_str("req: Lua timeout -> nil", sent_str(0, prj), "resp:192.168.1.20:00000002:nil");
    check_str("req: checkAlive() while busy -> false", sent_str(1, prj), "resp:192.168.1.20:00000003:false");
    s_lua_rc = ESP_OK;

    int calls = s_lua_calls;
    sent_reset();
    rx_reset();
    rx_add_str(prj, "req:no-colon", "192.168.1.50");
    rx_add_str(prj, "req:", "192.168.1.50");
    run_task(udp_task);
    check("req: malformed (no session / script) -> no call, no answer", s_lua_calls == calls && s_nsent == 0);

    // Unprivileged (not project / temp key): only checkAlive()
    clu_cipher_t k = *prj;
    sent_reset();
    dispatch_str("req:1.2.3.4:00000009:os.exit()", &k, false, false, false, "192.168.1.60");
    dispatch_str("req:1.2.3.4:0000000a:CHECKALIVE()", &k, false, false, false, "192.168.1.60");
    check_str("unprivileged req: script refused", sent_str(0, &k), "resp:ERROR");
    check_str("unprivileged req: checkAlive() allowed (case-insensitive)", sent_str(1, &k),
              "resp:192.168.1.20:0000000a:42");
}

static void test_project_commands(void)
{
    clu_cipher_t *prj = &g_clu.project;
    clu_cipher_t pk = pin_key();

    sent_reset();
    rx_reset();
    rx_add_str(prj, "req_start_ftp", "192.168.1.50");
    rx_add_str(prj, "req_tftp_stop", "192.168.1.50");
    run_task(udp_task);
    check_str("req_start_ftp -> resp:OK", sent_str(0, prj), "resp:OK");
    check_str("req_tftp_stop -> resp:OK", sent_str(1, prj), "resp:OK");
    check("TFTP disabled again by req_tftp_stop", s_tftp_on == 0);
    dispatch_str("req_start_ftp", prj, true, false, false, "192.168.1.50");
    check("req_start_ftp: TFTP enabled for the requester only",
          s_tftp_on == 1 && s_tftp_peer == inet_addr("192.168.1.50"));

    // project-key-only commands under the PIN key -> resp:ERROR, not executed
    s_tftp_on = -1;
    s_refreshes = 0;
    g_clu.has_temp = true; // re-keyed via discovery
    pk = pin_key();
    sent_reset();
    rx_reset();
    rx_add_str(&pk, "req_start_ftp", "192.168.1.50");
    rx_add_str(&pk, "req_refresh_modules", "192.168.1.50");
    run_task(udp_task);
    check("project-only commands under the PIN key: refused",
          s_tftp_on == -1 && s_refreshes == 0 && s_nsent == 2 && strcmp(sent_str(0, &pk), "resp:ERROR") == 0 &&
              strcmp(sent_str(1, &pk), "resp:ERROR") == 0);

    // diagnostic refresh
    sent_reset();
    rx_reset();
    rx_add_str(prj, "req_diagnostic_refresh:192.168.1.20:0000beef\r\n", "192.168.1.50");
    run_task(udp_task);
    check_str("req_diagnostic_refresh -> OK", sent_str(0, prj), "resp_diagnostic_refresh:0000beef:OK");
    check("diagnostic pack written with our IP, TFTP opened for the requester",
          strcmp(s_diag_ip, "192.168.1.20") == 0 && s_tftp_on == 1 && s_tftp_peer == inet_addr("192.168.1.50"));
    s_diag_err = 5;
    s_tftp_on = -1;
    sent_reset();
    rx_reset();
    rx_add_str(prj, "req_diagnostic_refresh:192.168.1.20:0000beef", "192.168.1.50");
    rx_add_str(prj, "req_diagnostic_refresh:192.168.1.20:0123456789abcdef0", "192.168.1.50");
    run_task(udp_task);
    check_str("req_diagnostic_refresh, write error -> ERROR:<n>", sent_str(0, prj),
              "resp_diagnostic_refresh:0000beef:ERROR:5");
    check("diagnostic error: TFTP stays closed; overlong session ignored", s_tftp_on == -1 && s_nsent == 1);
    s_diag_err = 0;

    // module refresh
    sent_reset();
    rx_reset();
    rx_add_str(prj, "req_refresh_modules:192.168.1.20", "192.168.1.50");
    run_task(udp_task);
    check("req_refresh_modules: bus rescanned (25 s timeout)", s_refreshes == 1 && s_refresh_timeout == 25000);
    check_str("req_refresh_modules -> resp_refresh_modules:<ip>", sent_str(0, prj),
              "resp_refresh_modules:192.168.1.20");
    s_refresh_ok = false;
    sent_reset();
    rx_reset();
    rx_add_str(prj, "req_refresh_modules", "192.168.1.50");
    run_task(udp_task);
    check_str("req_refresh_modules timeout: still answered", sent_str(0, prj), "resp_refresh_modules:192.168.1.20");
    s_refresh_ok = true;

    // unknown / non-command plaintext
    sent_reset();
    rx_reset();
    rx_add_str(prj, "req_foo", "192.168.1.50");
    rx_add_str(prj, "hello", "192.168.1.50");
    rx_add_raw((const uint8_t *)"0123456789abcdef!", 17, "192.168.1.50"); // no key decrypts it
    run_task(udp_task);
    check("unknown req -> resp:ERROR; non-command plaintext and undecryptable data -> silence (no padding oracle)",
          s_nsent == 1 && strcmp(sent_str(0, prj), "resp:ERROR") == 0);
}

static void test_reset(void)
{
    clu_cipher_t *prj = &g_clu.project;
    sent_reset();
    rx_reset();
    rx_add_str(prj, "req_reset:192.168.1.20", "192.168.1.50");
    run_task(udp_task);
    check_str("req_reset -> resp_reset:<ip> under the project key", sent_str(0, prj), "resp_reset:192.168.1.20");
    check("req_reset: soft reset (Lua reload), no reboot", s_reloads == 1 && s_halts == 0 && s_restarts == 0);

    s_reload_ok = false;
    rx_reset();
    rx_add_str(prj, "req_reset:192.168.1.20", "192.168.1.50");
    run_task(udp_task);
    check("req_reset, soft reset failed: Lua halted (storage flushed), then reboot",
          s_reloads == 2 && s_halts == 1 && s_restarts == 1);
    s_reload_ok = true;
}

static void test_report(void)
{
    sent_reset();
    udp_server_send_report("192.168.1.99", 6000, "{\"x\":1}");
    udp_server_send_report("not-an-ip", 6000, "x");
    check("report: sent once (bad address dropped)",
          s_nsent == 1 && s_sent[0].to.sin_addr.s_addr == inet_addr("192.168.1.99") &&
              s_sent[0].to.sin_port == htons(6000));
    check_str("report: resp:0.0.0.0:00000000:<body> under the project key", sent_str(0, &g_clu.project),
              "resp:0.0.0.0:00000000:{\"x\":1}");

    s_send_fail = true;
    udp_server_send_report("192.168.1.99", 6000, "x"); // sendto error: logged only
    s_send_fail = false;
    check("report: sendto failure tolerated", s_nsent == 1);
}

static void test_remote(void)
{
    const uint32_t peer = 0xc0a80115; // 192.168.1.21
    uint32_t id1 = udp_server_remote_exec(peer, "DOU1:get(0)");
    uint32_t id2 = udp_server_remote_exec(peer, "x()");
    uint32_t id3 = udp_server_remote_exec(peer, "y()");
    uint32_t id4 = udp_server_remote_exec(peer, "z()");
    uint32_t id5 = udp_server_remote_exec(peer, "w()");
    check("remote exec: increasing call ids", id1 && id2 == id1 + 1 && id5 == id1 + 4);

    // 1: noise, then the right answer; 2: timeout; 3: socket error; 4: send fails; 5: wrong sessions
    rx_reset();
    rx_add_str(&g_clu.project, "resp:192.168.1.21:00000000:1", "192.168.1.99"); // other host
    rx_add_raw((const uint8_t *)"garbage", 7, "192.168.1.21");                    // undecryptable
    rx_add_kind(RX_EAGAIN);                                                       // 200 ms slice
    rx_add_reply("192.168.1.21", "nope:%s:1");                                    // not a resp
    rx_add_reply("192.168.1.21", "resp:192.168.1.21:ffffffff:1");                 // other session
    rx_add_reply("192.168.1.21", "resp:192.168.1.21:%s:17");
    s_rx_timeout_slices = true;
    s_nposted = 0;
    sent_reset();
    s_next_fd = 10;

    // run_task returns when the remote queue is empty; drive the script per call
    remote_req_t r;
    int sock = 10;
    xQueueReceive(s_remote_q, &r, 0);
    remote_call(sock, &r);
    free(r.script);
    char req[256];
    sent_plain(0, &g_clu.project, req, sizeof req);
    check("remote: req:<our ip>:<session>:<script> to the peer's port 1234",
          strncmp(req, "req:192.168.1.20:", 17) == 0 && strstr(req, ":DOU1:get(0)") &&
              s_sent[0].to.sin_addr.s_addr == htonl(peer) && s_sent[0].to.sin_port == htons(CLU_UDP_PORT));
    check("remote: answer matched by host + session, noise skipped",
          s_nposted == 1 && s_posted_id[0] == id1 && strcmp(s_posted[0], "17") == 0);

    rx_reset(); // nothing arrives: 1.5 s of SO_RCVTIMEO slices
    TickType_t t0 = host_ticks;
    xQueueReceive(s_remote_q, &r, 0);
    remote_call(sock, &r);
    free(r.script);
    check("remote: no answer within 1.5 s -> nil",
          s_nposted == 2 && s_posted_id[1] == id2 && strcmp(s_posted[1], "(nil)") == 0 && host_ticks - t0 >= 1500);

    rx_reset();
    rx_add_kind(RX_ERR);
    xQueueReceive(s_remote_q, &r, 0);
    remote_call(sock, &r);
    free(r.script);
    check("remote: socket error -> nil", s_nposted == 3 && s_posted_id[2] == id3 && strcmp(s_posted[2], "(nil)") == 0);

    s_send_fail = true;
    xQueueReceive(s_remote_q, &r, 0);
    remote_call(sock, &r);
    free(r.script);
    s_send_fail = false;
    check("remote: send failure -> nil", s_nposted == 4 && s_posted_id[3] == id4 && strcmp(s_posted[3], "(nil)") == 0);

    // remaining call through the task loop
    rx_reset();
    rx_add_reply("192.168.1.21", "resp:192.168.1.21:%s:ok");
    run_task(remote_task);
    check("remote task: queued call executed, socket bound", s_nposted == 5 && strcmp(s_posted[4], "ok") == 0);

    // too long for one datagram
    char *big = malloc(BUF_SZ + 10);
    memset(big, 'a', BUF_SZ + 9);
    big[BUF_SZ + 9] = 0;
    uint32_t idb = udp_server_remote_exec(peer, big);
    free(big);
    sent_reset();
    run_task(remote_task);
    check("remote: script too long -> nil, nothing sent",
          s_nposted == 6 && s_posted_id[5] == idb && strcmp(s_posted[5], "(nil)") == 0 && s_nsent == 0);

    // queue full
    int ok = 0;
    for (int i = 0; i < REMOTE_QUEUE + 1; i++) {
        ok += udp_server_remote_exec(peer, "q()") != 0;
    }
    check("remote: queue full -> 0 (script freed)", ok == REMOTE_QUEUE);

    // socket failure: the task keeps draining the queue, answering nil
    s_sock_fail = true;
    s_nposted = 0;
    run_task(remote_task);
    s_sock_fail = false;
    check("remote task without a socket: queued calls answered nil", s_nposted == REMOTE_QUEUE &&
                                                                         strcmp(s_posted[0], "(nil)") == 0);
    s_rx_timeout_slices = false;
}

// After CONFIG_CLU_SETUP_WINDOW_S: no discovery, no re-keying / set_ip / TFTP, PIN key ignored.
static void test_setup_window(void)
{
    clu_cipher_t *prj = &g_clu.project;
    uint8_t rnd[30] = {0}, iv[16] = {0}, pkt[128];
    char req[96];
    snprintf(req, sizeof req, "req_set_clu_ip:%08lx:192.168.1.77:192.168.1.1", (unsigned long)g_clu.sn);

    s_now_us = (int64_t)CONFIG_CLU_SETUP_WINDOW_S * 1000000 - 1;
    xQueueReset(s_discovery_q);
    size_t n = discovery_req(pkt, prj, rnd, iv);
    rx_reset();
    rx_add(broadcast_key(), pkt, n, "192.168.1.50");
    run_task(udp_task);
    check("setup window: discovery accepted just before it closes", s_discovery_q->n == 1);
    xQueueReset(s_discovery_q);

    s_now_us = (int64_t)CONFIG_CLU_SETUP_WINDOW_S * 1000000;
    clu_cipher_t pk = pin_key(); // has_temp still set from the discovery above
    uint8_t key[16] = {0x55}, kpkt[96];
    size_t kn = set_key_req(kpkt, key, 16, iv);
    int saved = s_saved_keys, applied = s_net_applied;
    s_tftp_on = -1;
    s_lua_calls = 0;
    sent_reset();
    rx_reset();
    rx_add(broadcast_key(), pkt, n, "192.168.1.50");
    rx_add_str(broadcast_key(), req, "192.168.1.50");
    run_task(udp_task);
    check("window closed: discovery and broadcast set_ip silently ignored",
          s_discovery_q->n == 0 && s_nsent == 0 && s_net_applied == applied);

    rx_reset();
    rx_add(&pk, kpkt, kn, "192.168.1.50");
    rx_add_str(&pk, "req:1.2.3.4:00000001:x=1", "192.168.1.50");
    run_task(udp_task);
    check("window closed: PIN key no longer decrypts anything",
          s_nsent == 0 && s_saved_keys == saved && s_lua_calls == 0);

    rx_reset();
    rx_add(prj, kpkt, kn, "192.168.1.50");
    rx_add_str(prj, req, "192.168.1.50");
    rx_add_str(prj, "req_start_ftp", "192.168.1.50");
    rx_add_str(prj, "req_diagnostic_refresh:192.168.1.20:0000beef", "192.168.1.50");
    run_task(udp_task);
    check("window closed: set_key, set_ip, start_ftp, diagnostic_refresh refused under the project key",
          s_nsent == 4 && s_saved_keys == saved && s_net_applied == applied && s_tftp_on == -1);
    for (int i = 0; i < 4; i++) {
        check_str("window closed: refusal is resp:ERROR", sent_str(i, prj), "resp:ERROR");
    }

    sent_reset();
    rx_reset();
    rx_add_str(prj, "req_tftp_stop", "192.168.1.50");
    rx_add_str(prj, "req:1.2.3.4:00000002:checkAlive()", "192.168.1.50");
    run_task(udp_task);
    check("window closed: req_tftp_stop and req: still served", s_tftp_on == 0 && s_lua_calls == 1 &&
                                                                 s_nsent == 2 && strcmp(sent_str(0, prj), "resp:OK") == 0);

    // No button (-1): ticks only watch the window
    setup_tick(-1);
    check("setup tick without a button: window stays closed", !setup_window_open());

    // Setup button: a glitch (2 low samples) does nothing, a 100 ms hold reopens the window
    s_now_us = 1000LL * 1000000;
    setup_tick(0);
    setup_tick(0);
    setup_tick(1);
    setup_tick(0);
    check("setup button: short glitch ignored", !setup_window_open());
    setup_tick(0);
    setup_tick(0);
    check("setup button: held 3 samples -> window open", setup_window_open());
    s_now_us += (int64_t)CONFIG_CLU_SETUP_WINDOW_S * 1000000 - 1;
    setup_tick(0); // still held: no extension
    check("setup button: open for CLU_SETUP_WINDOW_S, holding does not extend it", setup_window_open());
    s_now_us += 1;
    setup_tick(0);
    check("setup button: window closes again", !setup_window_open());
    setup_tick(1);
    xQueueReset(s_discovery_q);
    rx_reset();
    rx_add(broadcast_key(), pkt, n, "192.168.1.50");
    run_task(udp_task);
    check("setup button: closed window still refuses discovery", s_discovery_q->n == 0);
    setup_tick(0);
    setup_tick(0);
    setup_tick(0);
    rx_reset();
    rx_add(broadcast_key(), pkt, n, "192.168.1.50");
    run_task(udp_task);
    check("setup button: new press -> discovery answered", s_discovery_q->n == 1);
    xQueueReset(s_discovery_q);

    // Emergency mode: open with no time limit, closes when the CLU leaves it
    s_now_us += 100LL * 24 * 3600 * 1000000;
    setup_tick(1);
    check("emergency: window closed before", !setup_window_open());
    s_lua_state = CLU_LUA_EMERGENCY;
    setup_tick(1);
    s_now_us += 30LL * 24 * 3600 * 1000000;
    setup_tick(1);
    rx_reset();
    rx_add(broadcast_key(), pkt, n, "192.168.1.50");
    run_task(udp_task);
    check("emergency: window open indefinitely, discovery answered", setup_window_open() && s_discovery_q->n == 1);
    xQueueReset(s_discovery_q);
    s_lua_state = CLU_LUA_OK;
    setup_tick(1);
    check("emergency over (soft reset with a fixed script): window closed again", !setup_window_open());
    s_now_us = 0;
    s_tftp_on = -1;
}

int main(void)
{
    g_clu.sn = 0x0c0ffee1;
    strcpy(g_clu.mac, "A1B2C3D4E5F6");
    strcpy(g_clu.pin, "PIN12345");
    for (int i = 0; i < 16; i++) {
        g_clu.project.key[i] = (uint8_t)(0x80 + i);
        g_clu.project.iv[i] = (uint8_t)(0x20 + i);
    }

    test_before_start();
    test_start_and_socket_errors();
    udp_server_set_local_ip(inet_addr("192.168.1.20"));
    check("local IP stored as text", strcmp(s_ip, "192.168.1.20") == 0);
    test_discovery();
    test_set_ip();
    test_set_key();
    test_lua_requests();
    test_project_commands();
    test_reset();
    test_report();
    test_remote();
    test_setup_window();
    check("tx lock released", host_mutex_held == 0);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
