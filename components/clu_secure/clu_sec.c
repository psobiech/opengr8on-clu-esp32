/**
 * @file clu_sec.c
 * @brief Secure CLU channel service: socket, tasks, NVS key / pins, outbound calls (see clu_sec.h).
 *
 * Threads:
 *  - sec_rx:   owns the socket; feeds the engine, routes probe answers / replies / handshake
 *              results to the waiting caller, queues inbound requests, ticks the engine.
 *  - sec_work: runs inbound requests (Lua) so a slow script never stalls the receive path.
 *  - caller:   clu_sec_call() from the remote-call task (one at a time).
 * The engine and the pins are guarded by s_lock.
 */
#include "clu_sec.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "clu_sec_engine.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "sec";

#ifndef CONFIG_CLU_SEC_PORT
#define CONFIG_CLU_SEC_PORT 1235 // CLU_SECURE_CHANNEL off: compiled, never started
#endif

#define NVS_NS        "sec"
#define LEGACY_SLOTS  16
#define LEGACY_TTL_MS (10u * 60u * 1000u)
#define PROBE_WAIT_MS 400
#define PROBE_TRIES   3
#define HS_WAIT_MS    1500 // per copy of HS_INIT; the responder runs 3 X25519 before answering
#define WORK_QUEUE    4
#define RX_BUF        1600

// Application messages inside DATA records (binary, see SECURE.md).
#define APP_REQ          1 // type call_id(4 LE) script
#define APP_RESP         2 // type call_id(4 LE) status(1) value
#define APP_ACK          3 // type call_id(4 LE)
#define APP_OK           0
#define APP_ERR          1
#define APP_HDR          5
#define REQ_RETRY_MS     300  // REQ resent until ACKed / answered
#define REQ_RETRY_ACKED  2000 // after the ACK: only to recover a lost RESP from the cache
#define DEDUP_SLOTS      4
#define DEDUP_TTL_MS     30000u
#define RESP_MAX         1400

/** @brief What the caller of clu_sec_call() is waiting for. */
typedef enum { W_NONE, W_PROBE, W_HS, W_RESP } wait_kind_t;

typedef struct {
    wait_kind_t kind;
    uint32_t peer_ip; // host order
    uint32_t nonce;
    uint32_t call_id;
    bool acked;
    uint8_t status;
    size_t rlen;
    bool succeeded;
    uint32_t serial;
    uint8_t pub[NOISE_KEY_LEN];
    char *out;
    size_t cap;
} waiter_t;

typedef struct {
    uint32_t handle;
    uint32_t peer_ip; // host order
    uint32_t call_id;
    char *script;
} work_t;

/** @brief Responder-side record of a call: a duplicate REQ is never executed twice. */
typedef struct {
    bool used;
    bool done;
    uint32_t peer_ip;
    uint32_t call_id;
    uint32_t until_ms;
    uint8_t *resp; // cached RESP (when done)
    size_t resp_len;
} dedup_t;

typedef struct {
    uint32_t peer_ip;
    uint32_t until_ms;
} legacy_t;

static clu_sec_cfg_t s_cfg;
static sec_engine_t s_eng;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_wait_sem;
static QueueHandle_t s_work_q;
static waiter_t s_wait;
static legacy_t s_legacy[LEGACY_SLOTS];
static dedup_t s_dedup[DEDUP_SLOTS];
static uint32_t s_call_seq;
static int s_sock = -1;
static bool s_active;

static bool load_keys(noise_keypair_t *keypair, sec_pins_t *pins);
static void op_deliver(void *ctx, uint32_t handle, const sec_session_t *session, const uint8_t *plain, size_t len);
static uint32_t rd32le(const uint8_t *bytes);
static dedup_t *dedup_alloc(void);
static void op_handshake_done(void *ctx, uint32_t peer_ip, bool succeeded);
static bool op_pairing_allowed(void *ctx);
static void op_pins_changed(void *ctx);
static void save_pins(void);
static uint64_t op_wall_ms(void *ctx);
static void op_random(void *ctx, uint8_t *out, size_t len);
static void rx_task(void *arg);
static void work_task(void *arg);
static dedup_t *dedup_find(uint32_t peer_ip, uint32_t call_id, uint32_t now);
static void dedup_clear(dedup_t *entry);
static void send_err(uint32_t handle, uint32_t call_id);
static void wr32le(uint8_t *bytes, uint32_t value);
static void app_send(uint32_t handle, const uint8_t *msg, size_t len);
static sec_pin_t *clu_pin_by_ip(uint32_t peer_ip);
static bool legacy_cached(uint32_t peer_ip, uint32_t now);
static void mark_legacy(uint32_t peer_ip, uint32_t now);
static bool probe(uint32_t peer_ip, uint32_t *serial, uint8_t pub[NOISE_KEY_LEN]);
static void op_send(void *ctx, uint32_t peer_ip, uint16_t port, const uint8_t *buf, size_t len);
static const char *ip_str(uint32_t ip_host, char buf[16]);
static bool handshake(uint32_t peer_ip, const uint8_t pub[NOISE_KEY_LEN]);
static uint32_t now_ms(void);
static void arm(wait_kind_t kind, uint32_t peer_ip);
static bool wait_for(uint32_t ms);
static void lock(void);
static void unlock(void);

bool clu_sec_start(const clu_sec_cfg_t *cfg)
{
    if (s_active) {
        return true;
    }
    s_cfg = *cfg;
    noise_keypair_t keypair;
    static sec_pins_t pins; // large: keep it off the caller's stack
    if (!load_keys(&keypair, &pins)) {
        ESP_LOGE(TAG, "static key unavailable, secure channel disabled");
        return false;
    }
    s_lock = xSemaphoreCreateMutex();
    s_wait_sem = xSemaphoreCreateBinary();
    s_work_q = xQueueCreate(WORK_QUEUE, sizeof(work_t));
    sec_ops_t ops = {op_send, op_deliver, op_handshake_done, op_pairing_allowed, op_pins_changed, op_random, NULL, op_wall_ms};
    s_call_seq = esp_random();
    sec_engine_init(&s_eng, &keypair, cfg->serial_number, &ops);
    s_eng.pins = pins;
    noise_wipe(&keypair, sizeof keypair);

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(CONFIG_CLU_SEC_PORT),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (s_sock < 0 || bind(s_sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        ESP_LOGE(TAG, "socket/bind udp/%d failed, secure channel disabled", CONFIG_CLU_SEC_PORT);
        if (s_sock >= 0) {
            close(s_sock);
            s_sock = -1;
        }
        return false;
    }
    struct timeval receive_timeout = {.tv_sec = 0, .tv_usec = 200 * 1000};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof receive_timeout);
    xTaskCreatePinnedToCore(rx_task, "sec_rx", 6144, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(work_task, "sec_work", 6144, NULL, 4, NULL, 0);
    s_active = true;
    char fingerprint_text[17];
    sec_fingerprint(s_eng.local_static.pub, fingerprint_text);
    int npins = 0;
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_PINS; i++) {
        npins += s_eng.pins.entries[i].flags != 0;
    }
    ESP_LOGI(TAG, "listening on udp/%d, key %s, %d pinned peer(s)", CONFIG_CLU_SEC_PORT, fingerprint_text, npins);
    return true;
}

bool clu_sec_active(void)
{
    return s_active;
}

clu_sec_result_t clu_sec_call(uint32_t peer_ip, const char *script, char *out, size_t cap, uint32_t timeout_ms)
{
    if (!s_active) {
        return CLU_SEC_LEGACY;
    }
    char ipb[16];
    uint32_t now = now_ms();
    uint8_t pub[NOISE_KEY_LEN];
    uint32_t peer_sn = 0;
    bool rekey = false;

    lock();
    sec_pin_t *pin = clu_pin_by_ip(peer_ip);
    bool pinned = pin != NULL;
    if (pinned) {
        memcpy(pub, pin->pub, sizeof pub);
        peer_sn = pin->serial;
    }
    const sec_session_t *session = sec_engine_session_for(&s_eng, peer_ip, now, &rekey);
    uint32_t handle = session ? session->local_idx : 0;
    bool cached = !pinned && legacy_cached(peer_ip, now);
    unlock();
    if (cached) {
        return CLU_SEC_LEGACY;
    }

    if (!handle || rekey) {
        bool candidate = false;
        if (!pinned) {
            if (!probe(peer_ip, &peer_sn, pub)) {
                ESP_LOGI(TAG, "%s: no answer on udp/%d, legacy CLU", ip_str(peer_ip, ipb), CONFIG_CLU_SEC_PORT);
                mark_legacy(peer_ip, now_ms());
                return CLU_SEC_LEGACY;
            }
            lock();
            bool known = sec_pins_by_pub(&s_eng.pins, pub) != NULL;
            bool conflict = !known && sec_pins_by_sn(&s_eng.pins, peer_sn) != NULL;
            unlock();
            if (conflict) {
                ESP_LOGW(TAG, "%s: CLU %lu presents a different key than pinned: refused (forget it to re-pair)",
                         ip_str(peer_ip, ipb), (unsigned long)peer_sn);
                return CLU_SEC_FAIL;
            }
            if (!known && !op_pairing_allowed(NULL)) {
                ESP_LOGW(TAG, "%s: CLU %lu not paired and the setup window is closed: legacy", ip_str(peer_ip, ipb),
                         (unsigned long)peer_sn);
                mark_legacy(peer_ip, now_ms());
                return CLU_SEC_LEGACY;
            }
            pinned = known;
            candidate = !known;
        }
        if (handshake(peer_ip, pub)) {
            bool changed = false;
            lock();
            sec_pin_result_t pin_result = sec_pins_check(&s_eng.pins, pub, peer_sn, peer_ip, SEC_PIN_CLU, candidate, &changed);
            if (changed) {
                save_pins();
            }
            unlock();
            if (pin_result == SEC_PIN_ADDED) {
                char fingerprint_text[17];
                sec_fingerprint(pub, fingerprint_text);
                ESP_LOGW(TAG, "%s: paired with CLU %lu (key %s)", ip_str(peer_ip, ipb), (unsigned long)peer_sn, fingerprint_text);
            } else if (pin_result == SEC_PIN_FULL) {
                ESP_LOGW(TAG, "%s: pin table full, CLU %lu not pinned", ip_str(peer_ip, ipb), (unsigned long)peer_sn);
            }
        } else if (!handle) {
            if (candidate) {
                ESP_LOGW(TAG, "%s: CLU %lu refused pairing (its setup window closed?): legacy", ip_str(peer_ip, ipb),
                         (unsigned long)peer_sn);
                mark_legacy(peer_ip, now_ms());
                return CLU_SEC_LEGACY;
            }
            ESP_LOGW(TAG, "%s: handshake with paired CLU failed", ip_str(peer_ip, ipb));
            return CLU_SEC_FAIL;
        }
        lock();
        session = sec_engine_session_for(&s_eng, peer_ip, now_ms(), NULL);
        handle = session ? session->local_idx : 0;
        unlock();
        if (!handle) {
            return CLU_SEC_FAIL;
        }
    }

    const size_t slen = strlen(script);
    uint8_t *req = malloc(APP_HDR + slen);
    if (!req) {
        return CLU_SEC_FAIL;
    }
    req[0] = APP_REQ;
    memcpy(req + APP_HDR, script, slen);

    lock();
    arm(W_RESP, peer_ip);
    s_wait.call_id = ++s_call_seq;
    s_wait.acked = false;
    s_wait.out = out;
    s_wait.cap = cap;
    wr32le(req + 1, s_wait.call_id);
    unlock();

    const uint32_t start = now_ms();
    uint32_t last_tx = 0;
    bool first = true, got = false;
    while (!got && (int32_t)(now_ms() - start) < (int32_t)timeout_ms) {
        lock();
        if (first || (int32_t)(now_ms() - last_tx) >= (s_wait.acked ? REQ_RETRY_ACKED : REQ_RETRY_MS)) {
            const sec_session_t *current_session = sec_engine_session_for(&s_eng, peer_ip, now_ms(), NULL);
            if (!current_session || sec_engine_send(&s_eng, current_session->local_idx, req, APP_HDR + slen, now_ms())) {
                if (first) {
                    unlock();
                    break;
                }
            }
            first = false;
            last_tx = now_ms();
        }
        unlock();
        got = xSemaphoreTake(s_wait_sem, pdMS_TO_TICKS(50)) == pdTRUE;
    }
    lock();
    bool succeeded = got && s_wait.succeeded && s_wait.status == APP_OK;
    s_wait.kind = W_NONE;
    unlock();
    free(req);
    return succeeded ? CLU_SEC_OK : CLU_SEC_FAIL;
}

bool clu_sec_is_pinned_ip(uint32_t peer_ip)
{
    if (!s_active) {
        return false;
    }
    lock();
    bool pinned = clu_pin_by_ip(peer_ip) != NULL;
    unlock();
    return pinned;
}

void clu_sec_fingerprint(char out[17])
{
    if (!s_active) {
        out[0] = 0;
        return;
    }
    sec_fingerprint(s_eng.local_static.pub, out);
}

size_t clu_sec_pubkey_text(char *out, size_t cap)
{
    if (!s_active || cap < 12 + 2 * NOISE_KEY_LEN) {
        return 0;
    }
    int written = snprintf(out, cap, "%lu:", (unsigned long)s_eng.serial);
    for (int i = 0; i < NOISE_KEY_LEN; i++) {
        written += snprintf(out + written, cap - (size_t)written, "%02x", s_eng.local_static.pub[i]);
    }
    return (size_t)written;
}

int clu_sec_forget(const char *what)
{
    if (!s_active) {
        return 0;
    }
    lock();
    int removed = sec_pins_forget(&s_eng.pins, what);
    if (removed) {
        save_pins();
        memset(s_legacy, 0, sizeof s_legacy);
    }
    unlock();
    if (removed) {
        ESP_LOGW(TAG, "forgot %d pinned key(s) matching %s", removed, what);
    }
    return removed;
}

size_t clu_sec_info(char *out, size_t cap)
{
    if (!s_active) {
        return (size_t)snprintf(out, cap, "secure channel inactive");
    }
    char fingerprint_text[17], ipb[16];
    lock();
    sec_fingerprint(s_eng.local_static.pub, fingerprint_text);
    size_t written = (size_t)snprintf(out, cap, "key %s, port %d, sessions %d", fingerprint_text, CONFIG_CLU_SEC_PORT,
                                sec_engine_session_count(&s_eng));
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_PINS && written < cap; i++) {
        const sec_pin_t *pin = &s_eng.pins.entries[i];
        if (!pin->flags) {
            continue;
        }
        sec_fingerprint(pin->pub, fingerprint_text);
        written += (size_t)snprintf(out + written, cap - written, "\n%s %lu %s %s", pin->flags & SEC_PIN_HOST ? "host" : "clu",
                              (unsigned long)pin->serial, pin->peer_ip ? ip_str(pin->peer_ip, ipb) : "-", fingerprint_text);
    }
    unlock();
    return written < cap ? written : cap - 1;
}

/** @brief Load the static key (create it on first boot) and the pins. */
static bool load_keys(noise_keypair_t *keypair, sec_pins_t *pins)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        return false;
    }
    uint8_t priv[NOISE_KEY_LEN];
    size_t len = sizeof priv;
    bool succeeded;
    if (nvs_get_blob(nvs_handle, "sk", priv, &len) == ESP_OK && len == sizeof priv) {
        succeeded = noise_keypair_from_priv(keypair, priv) == 0;
    } else {
        succeeded = noise_keypair_generate(keypair) == 0 && nvs_set_blob(nvs_handle, "sk", keypair->priv, sizeof keypair->priv) == ESP_OK &&
             nvs_commit(nvs_handle) == ESP_OK;
        if (succeeded) {
            ESP_LOGW(TAG, "new static key generated");
        }
    }
    noise_wipe(priv, sizeof priv);
    memset(pins, 0, sizeof *pins);
    len = sizeof *pins;
    if (nvs_get_blob(nvs_handle, "pins", pins, &len) == ESP_ERR_NVS_INVALID_LENGTH) {
        // CLU_SEC_MAX_PINS changed: keep what fits.
        size_t stored = 0;
        nvs_get_blob(nvs_handle, "pins", NULL, &stored);
        uint8_t *tmp = malloc(stored);
        if (tmp && nvs_get_blob(nvs_handle, "pins", tmp, &stored) == ESP_OK) {
            memcpy(pins, tmp, stored < sizeof *pins ? stored - stored % sizeof(sec_pin_t) : sizeof *pins);
        }
        free(tmp);
    }
    nvs_close(nvs_handle);
    return succeeded;
}

static void op_deliver(void *ctx, uint32_t handle, const sec_session_t *session, const uint8_t *plain, size_t len)
{
    (void)ctx;
    if (len < APP_HDR) {
        return;
    }
    const uint32_t incoming_call_id = rd32le(plain + 1);
    const uint32_t now = now_ms();
    switch (plain[0]) {
    case APP_ACK:
        if (s_wait.kind == W_RESP && session->peer_ip == s_wait.peer_ip && incoming_call_id == s_wait.call_id) {
            s_wait.acked = true;
        }
        break;
    case APP_RESP:
        if (len > APP_HDR && s_wait.kind == W_RESP && session->peer_ip == s_wait.peer_ip && incoming_call_id == s_wait.call_id && s_wait.cap) {
            size_t copy_len = len - APP_HDR - 1;
            copy_len = copy_len < s_wait.cap - 1 ? copy_len : s_wait.cap - 1;
            memcpy(s_wait.out, plain + APP_HDR + 1, copy_len);
            s_wait.out[copy_len] = 0;
            s_wait.rlen = copy_len;
            s_wait.status = plain[APP_HDR];
            s_wait.kind = W_NONE;
            s_wait.succeeded = true;
            xSemaphoreGive(s_wait_sem);
        }
        break;
    case APP_REQ: {
        dedup_t *dedup_entry = dedup_find(session->peer_ip, incoming_call_id, now);
        uint8_t ack[APP_HDR] = {APP_ACK};
        wr32le(ack + 1, incoming_call_id);
        if (dedup_entry) {
            app_send(handle, ack, sizeof ack);
            if (dedup_entry->done && dedup_entry->resp) {
                app_send(handle, dedup_entry->resp, dedup_entry->resp_len);
            }
            break;
        }
        dedup_entry = dedup_alloc();
        work_t work = {.handle = handle, .peer_ip = session->peer_ip, .call_id = incoming_call_id, .script = malloc(len - APP_HDR + 1)};
        if (!dedup_entry || !work.script) {
            free(work.script);
            send_err(handle, incoming_call_id);
            break;
        }
        memcpy(work.script, plain + APP_HDR, len - APP_HDR);
        work.script[len - APP_HDR] = 0;
        if (xQueueSend(s_work_q, &work, 0) != pdTRUE) {
            ESP_LOGW(TAG, "request queue full, rejected");
            free(work.script);
            send_err(handle, incoming_call_id);
            break;
        }
        dedup_entry->used = true;
        dedup_entry->done = false;
        dedup_entry->peer_ip = session->peer_ip;
        dedup_entry->call_id = incoming_call_id;
        app_send(handle, ack, sizeof ack);
        break;
    }
    default:
        break;
    }
}

static uint32_t rd32le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/** @brief Free slot, or the oldest finished one (s_lock held); NULL when all are running. */
static dedup_t *dedup_alloc(void)
{
    dedup_t *best = NULL;
    for (int i = 0; i < DEDUP_SLOTS; i++) {
        dedup_t *entry = &s_dedup[i];
        if (!entry->used) {
            return entry;
        }
        if (entry->done && (!best || (int32_t)(entry->until_ms - best->until_ms) < 0)) {
            best = entry;
        }
    }
    if (best) {
        dedup_clear(best);
    }
    return best;
}

static void op_handshake_done(void *ctx, uint32_t peer_ip, bool succeeded)
{
    (void)ctx;
    if (s_wait.kind == W_HS && s_wait.peer_ip == peer_ip) {
        s_wait.kind = W_NONE;
        s_wait.succeeded = succeeded;
        xSemaphoreGive(s_wait_sem);
    }
}

static bool op_pairing_allowed(void *ctx)
{
    (void)ctx;
    return s_cfg.pairing_allowed && s_cfg.pairing_allowed();
}

static void op_pins_changed(void *ctx)
{
    (void)ctx;
    save_pins();
}

/** @brief Persist the pin table (called with s_lock held). */
static void save_pins(void)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed, pins not saved");
        return;
    }
    if (nvs_set_blob(nvs_handle, "pins", &s_eng.pins, sizeof s_eng.pins) != ESP_OK || nvs_commit(nvs_handle) != ESP_OK) {
        ESP_LOGE(TAG, "pins not saved");
    }
    nvs_close(nvs_handle);
}

static uint64_t op_wall_ms(void *ctx)
{
    (void)ctx;
    return s_cfg.wall_ms ? s_cfg.wall_ms() : 0;
}

static void op_random(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
}

static void rx_task(void *arg)
{
    (void)arg;
    static uint8_t rx_buffer[RX_BUF];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int received_len = recvfrom(s_sock, rx_buffer, sizeof rx_buffer, 0, (struct sockaddr *)&from, &flen);
        uint32_t now = now_ms();
        lock();
        if (received_len > 0) {
            uint32_t peer_ip = ntohl(from.sin_addr.s_addr);
            uint32_t serial;
            uint8_t pub[NOISE_KEY_LEN];
            if (rx_buffer[0] == SEC_T_PROBE_RESP) {
                if (s_wait.kind == W_PROBE && s_wait.peer_ip == peer_ip && sec_probe_parse(rx_buffer, (size_t)received_len, s_wait.nonce, &serial, pub)) {
                    s_wait.serial = serial;
                    memcpy(s_wait.pub, pub, sizeof pub);
                    s_wait.kind = W_NONE;
                    s_wait.succeeded = true;
                    xSemaphoreGive(s_wait_sem);
                }
            } else {
                sec_engine_input(&s_eng, peer_ip, ntohs(from.sin_port), rx_buffer, (size_t)received_len, now);
            }
        }
        sec_engine_tick(&s_eng, now);
        unlock();
    }
}

static void work_task(void *arg)
{
    (void)arg;
    static char val[RESP_MAX];
    for (;;) {
        work_t work;
        xQueueReceive(s_work_q, &work, portMAX_DELAY);
        size_t response_len = s_cfg.handle_request ? s_cfg.handle_request(work.script, htonl(work.peer_ip), val, sizeof val) : 0;
        free(work.script);
        if (response_len > sizeof val) {
            response_len = sizeof val;
        }
        uint8_t *msg = malloc(APP_HDR + 1 + response_len);
        lock();
        dedup_t *dedup_entry = dedup_find(work.peer_ip, work.call_id, now_ms());
        if (!msg) {
            if (dedup_entry) {
                dedup_clear(dedup_entry);
            }
            send_err(work.handle, work.call_id);
        } else {
            msg[0] = APP_RESP;
            wr32le(msg + 1, work.call_id);
            msg[APP_HDR] = APP_OK;
            memcpy(msg + APP_HDR + 1, val, response_len);
            if (dedup_entry) {
                dedup_entry->done = true;
                dedup_entry->until_ms = now_ms() + DEDUP_TTL_MS;
                dedup_entry->resp = msg;
                dedup_entry->resp_len = APP_HDR + 1 + response_len;
            }
            app_send(work.handle, msg, APP_HDR + 1 + response_len);
            if (!dedup_entry) {
                free(msg);
            }
        }
        unlock();
    }
}

/** @brief Cache entry for (ip, call_id), expired ones are dropped on the way (s_lock held). */
static dedup_t *dedup_find(uint32_t peer_ip, uint32_t call_id, uint32_t now)
{
    dedup_t *hit = NULL;
    for (int i = 0; i < DEDUP_SLOTS; i++) {
        dedup_t *entry = &s_dedup[i];
        if (entry->used && entry->done && (int32_t)(now - entry->until_ms) > 0) {
            dedup_clear(entry);
        }
        if (entry->used && entry->peer_ip == peer_ip && entry->call_id == call_id) {
            hit = entry;
        }
    }
    return hit;
}

static void dedup_clear(dedup_t *entry)
{
    free(entry->resp);
    memset(entry, 0, sizeof *entry);
}

static void send_err(uint32_t handle, uint32_t call_id)
{
    uint8_t message[APP_HDR + 1] = {APP_RESP};
    wr32le(message + 1, call_id);
    message[APP_HDR] = APP_ERR;
    app_send(handle, message, sizeof message);
}

static void wr32le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

/** @brief Send a header-only or short app message on session @p handle (s_lock held). */
static void app_send(uint32_t handle, const uint8_t *msg, size_t len)
{
    if (sec_engine_send(&s_eng, handle, msg, len, now_ms())) {
        ESP_LOGW(TAG, "app message dropped: session gone");
    }
}

/** @brief Pinned CLU (not host tool) last seen at @p ip (s_lock held). */
static sec_pin_t *clu_pin_by_ip(uint32_t peer_ip)
{
    for (int i = 0; peer_ip && i < CONFIG_CLU_SEC_MAX_PINS; i++) {
        sec_pin_t *pin = &s_eng.pins.entries[i];
        if ((pin->flags & SEC_PIN_CLU) && pin->peer_ip == peer_ip) {
            return pin;
        }
    }
    return NULL;
}

static bool legacy_cached(uint32_t peer_ip, uint32_t now)
{
    for (int i = 0; i < LEGACY_SLOTS; i++) {
        if (s_legacy[i].peer_ip == peer_ip && (int32_t)(s_legacy[i].until_ms - now) > 0) {
            return true;
        }
    }
    return false;
}

static void mark_legacy(uint32_t peer_ip, uint32_t now)
{
    int slot = 0;
    for (int i = 0; i < LEGACY_SLOTS; i++) {
        if (s_legacy[i].peer_ip == peer_ip || (int32_t)(s_legacy[i].until_ms - now) <= 0) {
            slot = i;
            break;
        }
        if ((int32_t)(s_legacy[i].until_ms - s_legacy[slot].until_ms) < 0) {
            slot = i;
        }
    }
    s_legacy[slot].peer_ip = peer_ip;
    s_legacy[slot].until_ms = now + LEGACY_TTL_MS;
}

/** @brief PROBE @p ip (two tries): its SN and static key. */
static bool probe(uint32_t peer_ip, uint32_t *serial, uint8_t pub[NOISE_KEY_LEN])
{
    for (int attempt = 0; attempt < PROBE_TRIES; attempt++) {
        uint8_t pkt[SEC_PROBE_LEN];
        lock();
        arm(W_PROBE, peer_ip);
        esp_fill_random(&s_wait.nonce, sizeof s_wait.nonce);
        sec_probe_build(s_wait.nonce, pkt);
        op_send(NULL, peer_ip, CONFIG_CLU_SEC_PORT, pkt, sizeof pkt);
        unlock();
        if (wait_for(PROBE_WAIT_MS)) {
            lock();
            *serial = s_wait.serial;
            memcpy(pub, s_wait.pub, NOISE_KEY_LEN);
            unlock();
            return true;
        }
    }
    return false;
}

static void op_send(void *ctx, uint32_t peer_ip, uint16_t port, const uint8_t *buf, size_t len)
{
    (void)ctx;
    struct sockaddr_in destination = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(peer_ip)};
    if (sendto(s_sock, buf, len, 0, (struct sockaddr *)&destination, sizeof destination) < 0) {
        char ip_text[16];
        ESP_LOGW(TAG, "sendto %s failed: errno %d", ip_str(peer_ip, ip_text), errno);
    }
}

static const char *ip_str(uint32_t ip_host, char buf[16])
{
    struct in_addr address = {.s_addr = htonl(ip_host)};
    inet_ntoa_r(address, buf, 16);
    return buf;
}

/**
 * @brief Run a handshake with @p pub at @p ip: one HS_INIT, retransmitted once unchanged, so a slow
 * responder's answer to the first copy still counts.
 */
static bool handshake(uint32_t peer_ip, const uint8_t pub[NOISE_KEY_LEN])
{
    const uint32_t start = now_ms();
    for (int attempt = 0; attempt < 2; attempt++) {
        lock();
        arm(W_HS, peer_ip);
        int connect_result = attempt == 0 ? sec_engine_connect(&s_eng, peer_ip, CONFIG_CLU_SEC_PORT, pub, now_ms())
                             : sec_engine_retransmit(&s_eng, peer_ip);
        if (connect_result) {
            s_wait.kind = W_NONE;
        }
        unlock();
        if (connect_result) {
            return false;
        }
        if (wait_for(HS_WAIT_MS)) {
            return true;
        }
        // HS_RESP may have landed between two waits (no one armed): look at the sessions.
        lock();
        const sec_session_t *session = sec_engine_session_for(&s_eng, peer_ip, now_ms(), NULL);
        bool done = session && session->initiator && (int32_t)(session->created_ms - start) >= 0;
        unlock();
        if (done) {
            return true;
        }
    }
    return false;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/** @brief Arm the waiter (s_lock held), dropping a stale signal. */
static void arm(wait_kind_t kind, uint32_t peer_ip)
{
    xSemaphoreTake(s_wait_sem, 0);
    s_wait.kind = kind;
    s_wait.peer_ip = peer_ip;
    s_wait.succeeded = false;
}

/** @brief Wait for the armed event; disarms on timeout. */
static bool wait_for(uint32_t ms)
{
    bool got = xSemaphoreTake(s_wait_sem, pdMS_TO_TICKS(ms)) == pdTRUE;
    lock();
    bool succeeded = got && s_wait.succeeded;
    s_wait.kind = W_NONE;
    unlock();
    return succeeded;
}

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}
