/**
 * @file udp_server.c
 * @brief CLU UDP command server (port 1234). Port of src/vclu/Server.ts dispatch.
 *
 * Implemented: discovery, set_clu_ip (static IP, persisted), set_key, reset (Lua reload, see
 * CLU_SOFT_RESET), start/stop TFTP, req/resp executed in the Lua runtime (clu_lua), outgoing
 * remote-CLU calls (udp_server_remote_exec).
 */
#include "udp_server.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "clu_crypto.h"
#include "clu_keys.h"
#include "clu_lua.h"
#include "clu_net.h"
#include "clu_state.h"
#include "tfbus_master.h"
#include "clu_diag.h"
#include "clu_tftp.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "udp";
static int s_sock = -1;
static char s_ip[16] = "0.0.0.0";

#define BUF_SZ 1500
#define DISCOVERY_REPLY_COPIES 3
// Real CLU: every req_discovery_clu re-reads the
// TFBUS modules, rewrites a:\CONFIG.JSON and a:\config.txt, and only then answers, so OM's next
// step (TFTP of config.txt) sees the current module list. Clients resend discovery (OM's 30 s
// window, the Node client every 1 s): one scan serves every request within this window.
#define DISCOVERY_RESCAN_FRESH_MS 30000
#define DISCOVERY_RESCAN_TIMEOUT_MS 20000
#define DISCOVERY_QUEUE 8

/** @brief Discovery reply queued for discovery_task(), already built and keyed. */
typedef struct {
    uint8_t resp[128];
    size_t len;
    clu_cipher_t key;
    struct sockaddr_in to;
} discovery_reply_t;

static QueueHandle_t s_discovery_q;

#ifndef CONFIG_CLU_SETUP_BUTTON_GPIO
#define CONFIG_CLU_SETUP_BUTTON_GPIO (-1)
#endif
#define SETUP_BUTTON_POLL_MS 50
#define SETUP_BUTTON_HOLD_POLLS 3 // held low for >= 100 ms (3 samples) counts as a press

// End of the setup window [esp_timer us]. Open (not counting) until udp_server_start() arms it;
// written by udp_server_start() and the setup task, read by the UDP task.
static _Atomic int64_t s_setup_until_us = INT64_MAX;

/**
 * @brief True during the setup window: the first CONFIG_CLU_SETUP_WINDOW_S seconds after the CLU
 * is up (udp_server_start(): WiFi has an IP, every service started), or after a press of the
 * setup button, and for as long as the Lua runtime is in emergency mode (MAIN.LUA failed, so the
 * project must be re-uploaded) (0 = always).
 *
 * Discovery, re-keying (set_key, PIN / temp key), set_clu_ip and opening TFTP are only accepted
 * inside it. Armed once per chip boot, so a soft reset (req_reset, which needs only
 * a replayable project-key packet) does not reopen it; a reboot, power cycle or the button does.
 */
static bool setup_window_open(void)
{
#if CONFIG_CLU_SETUP_WINDOW_S > 0
    return clu_lua_state() == CLU_LUA_EMERGENCY || esp_timer_get_time() < atomic_load(&s_setup_until_us);
#else
    return true;
#endif
}

/**
 * @brief One setup-task tick: feed a button sample (0 = pressed, -1 = no button), log entering
 * emergency mode (window open with no time limit) and the window closing.
 *
 * A press held for SETUP_BUTTON_HOLD_POLLS samples reopens the window, once per press.
 */
static void setup_tick(int level)
{
    static int low;
    static bool was_open = true, was_emergency;
    bool emergency = clu_lua_state() == CLU_LUA_EMERGENCY;
    if (emergency && !was_emergency) {
        ESP_LOGW(TAG, "emergency mode: discovery / keys / TFTP open until the CLU leaves it");
    }
    was_emergency = emergency;
    if (level != 0) {
        low = 0;
    } else if (++low == SETUP_BUTTON_HOLD_POLLS) {
        atomic_store(&s_setup_until_us,
                     esp_timer_get_time() + (int64_t)CONFIG_CLU_SETUP_WINDOW_S * 1000000);
        ESP_LOGW(TAG, "setup button: discovery / keys / TFTP open for %d s", CONFIG_CLU_SETUP_WINDOW_S);
    }
    bool open = setup_window_open();
    if (was_open && !open) {
        ESP_LOGW(TAG, "setup window closed: discovery / keys / TFTP refused until the setup button "
                      "or a reset");
    }
    was_open = open;
}

/** @brief Task: poll the setup button (active low, internal pull-up) and watch the window. */
static void setup_task(void *arg)
{
    (void)arg;
    for (;;) {
        setup_tick(CONFIG_CLU_SETUP_BUTTON_GPIO >= 0 ? gpio_get_level(CONFIG_CLU_SETUP_BUTTON_GPIO) : -1);
        vTaskDelay(pdMS_TO_TICKS(SETUP_BUTTON_POLL_MS));
    }
}

/** @brief Configure the setup button GPIO (if any) and start the setup task (if the window is on). */
static void setup_start(void)
{
#if CONFIG_CLU_SETUP_WINDOW_S > 0
    // Count from here, not from the chip boot: waiting for WiFi must not use up the window.
    atomic_store(&s_setup_until_us, esp_timer_get_time() + (int64_t)CONFIG_CLU_SETUP_WINDOW_S * 1000000);
    ESP_LOGW(TAG, "setup window: discovery / keys / TFTP open for %d s", CONFIG_CLU_SETUP_WINDOW_S);
    if (CONFIG_CLU_SETUP_BUTTON_GPIO >= 0) {
        gpio_config_t c = {.pin_bit_mask = 1ULL << CONFIG_CLU_SETUP_BUTTON_GPIO,
                           .mode = GPIO_MODE_INPUT,
                           .pull_up_en = GPIO_PULLUP_ENABLE};
        if (gpio_config(&c) != ESP_OK) {
            ESP_LOGE(TAG, "setup button GPIO %d: config failed", CONFIG_CLU_SETUP_BUTTON_GPIO);
        }
    }
    xTaskCreatePinnedToCore(setup_task, "clu_setup", 2048, NULL, 2, NULL, 0);
#endif
}

/** @brief Log a command refused because the setup window is closed. */
static void log_locked(const char *what, const struct sockaddr_in *from)
{
    ESP_LOGW(TAG, "%s from %s refused: setup window closed (press the setup button or reset the CLU)",
             what, inet_ntoa(from->sin_addr));
}

/** @brief Grenton broadcast key + default broadcast IV (discovery requests). */
static const clu_cipher_t *broadcast_key(void)
{
    static clu_cipher_t k;
    memcpy(k.key, CLU_GRENTON_KEY, 16);
    memcpy(k.iv, CLU_DEFAULT_BROADCAST_IV, 16);
    return &k;
}

void udp_server_set_local_ip(uint32_t ip_be)
{
    struct in_addr a = {.s_addr = ip_be};
    inet_ntoa_r(a, s_ip, sizeof s_ip);
}

// send_enc runs in the UDP task and in the Lua task (clientReport via udp_server_send_report):
// the shared output buffer is guarded.
static SemaphoreHandle_t s_tx_lock;

/**
 * @brief Encrypt @p plain with @p key and send it to @p to.
 *
 * Runs in the UDP task and in the Lua task (clientReport via udp_server_send_report): the shared
 * output buffer is guarded by s_tx_lock.
 */
static void send_enc(const clu_cipher_t *key, const void *plain, size_t len,
                     const struct sockaddr_in *to)
{
    static uint8_t out[BUF_SZ + 16];
    if (!s_tx_lock) {
        return;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int n = clu_encrypt(key, plain, len, out, sizeof out);
    if (n <= 0) {
        ESP_LOGW(TAG, "encrypt failed (%d)", n);
    } else if (sendto(s_sock, out, n, 0, (const struct sockaddr *)to, sizeof *to) < 0) {
        ESP_LOGW(TAG, "sendto %s failed: errno %d", inet_ntoa(to->sin_addr), errno);
    }
    xSemaphoreGive(s_tx_lock);
}

/** @brief Encrypt and send a NUL-terminated string. */
static void send_str(const clu_cipher_t *key, const char *s, const struct sockaddr_in *to)
{
    send_enc(key, s, strlen(s), to);
}

/** @brief True if @p buf starts with "<prefix>:". */
static bool starts(const uint8_t *buf, size_t len, const char *prefix)
{
    size_t pl = strlen(prefix);
    return len > pl && memcmp(buf, prefix, pl) == 0 && buf[pl] == ':';
}

/**
 * @brief Match a keyed command; layout shared by discovery and set_key: enc(32) ':' iv(16) ':' cmd ':' ...
 *
 * @param buf      Plaintext packet.
 * @param len      Packet length.
 * @param cmd      Command name.
 * @param tail     Out: bytes after "cmd:".
 * @param tail_len Out: length of @p tail.
 * @return true if the packet is @p cmd.
 */
static bool keyed_cmd(const uint8_t *buf, size_t len, const char *cmd, const uint8_t **tail,
                      size_t *tail_len)
{
    const size_t hdr = CLU_RANDOM_ENC_BYTES + 1 + CLU_IV_BYTES + 1;
    size_t cl = strlen(cmd);
    if (len < hdr + cl + 1 || buf[32] != ':' || buf[49] != ':' ||
        memcmp(buf + hdr, cmd, cl) != 0 || buf[hdr + cl] != ':') {
        return false;
    }
    *tail = buf + hdr + cl + 1;
    *tail_len = len - (hdr + cl + 1);
    return true;
}

/**
 * @brief req_discovery_clu: prove knowledge of the device key and queue the reply.
 *
 * Decrypts OM's random with the project key, hashes it and encrypts the hash with the temp key
 * (default_key(PIN) + fresh temp IV). The reply itself is sent by discovery_task().
 */
static void on_discovery(const uint8_t *buf, const struct sockaddr_in *from)
{
    const uint8_t *enc = buf;
    const uint8_t *req_iv = buf + 33;

    uint8_t rnd[32];
    int rl = clu_decrypt(&g_clu.project, enc, 32, rnd, sizeof rnd);
    if (rl != CLU_RANDOM_BYTES) {
        // OM encrypts the random with ITS project key + IV: no match means OM and this CLU
        // disagree on the project key, so OM's follow-up (set_clu_ip) will not decrypt either.
        ESP_LOGW(TAG, "discovery: random not under our project key (OM has another project key)");
        clu_random(rnd, CLU_RANDOM_BYTES);
    }
    uint8_t h[CLU_RANDOM_BYTES];
    clu_discovery_hash(rnd, CLU_RANDOM_BYTES, h);

    // Temp key = the CLU's own device key (default_key(PIN)), not the project key, like the
    // vCLU. OM checks enc(hash) against default_key(private key it stores
    // for this SN) + our IV: a match ("known CLU") lets it re-key silently with that key. No
    // match (wrong key, or a hash not computed on signed bytes) puts the CLU in OM's "reset
    // project key" list, which prompts for the PIN on every discovery.
    if (!g_clu.has_temp) {
        clu_random(g_clu.temp.iv, 16);
        g_clu.has_temp = true;
    }
    clu_default_key((const uint8_t *)g_clu.pin, g_clu.temp.key);

    uint8_t resp[128];
    int el = clu_encrypt(&g_clu.temp, h, sizeof h, resp, sizeof resp);
    if (el != CLU_RANDOM_ENC_BYTES) {
        return;
    }
    size_t off = 32;
    resp[off++] = ':';
    memcpy(resp + off, g_clu.temp.iv, 16);
    off += 16;
    off += snprintf((char *)resp + off, sizeof resp - off, ":resp_discovery_clu:%08lx:%s",
                    (unsigned long)g_clu.sn, g_clu.mac);

    discovery_reply_t r = {.len = off, .key = *broadcast_key(), .to = *from};
    memcpy(r.resp, resp, off);
    memcpy(r.key.iv, req_iv, 16);
    if (!s_discovery_q || xQueueSend(s_discovery_q, &r, 0) != pdTRUE) {
        ESP_LOGW(TAG, "discovery from %s dropped (queue full)", inet_ntoa(from->sin_addr));
    }
}

/**
 * @brief Task: answer queued discovery requests.
 *
 * Rescans the bus (unless a scan finished within DISCOVERY_RESCAN_FRESH_MS), then sends
 * DISCOVERY_REPLY_COPIES copies of the reply.
 */
static void discovery_task(void *arg)
{
    (void)arg;
    TickType_t last_scan = 0;
    bool scanned = false;
    discovery_reply_t r;
    for (;;) {
        xQueueReceive(s_discovery_q, &r, portMAX_DELAY);
        if (!scanned || xTaskGetTickCount() - last_scan > pdMS_TO_TICKS(DISCOVERY_RESCAN_FRESH_MS)) {
            ESP_LOGI(TAG, "discovery from %s: re-reading TFBUS modules", inet_ntoa(r.to.sin_addr));
            if (!tfbus_master_refresh(DISCOVERY_RESCAN_TIMEOUT_MS)) {
                ESP_LOGW(TAG, "discovery: module rescan failed or timed out, answering anyway");
            }
            last_scan = xTaskGetTickCount();
            scanned = true;
        }
        // UDP has no delivery feedback (sendto only means "queued"), and on a lossy WiFi link the
        // single reply is often lost. The reply is idempotent (same temp IV, same hash), so send
        // a few copies spread over ~150 ms; clients key devices by serial number.
        static const uint16_t gap_ms[DISCOVERY_REPLY_COPIES] = {0, 50, 100};
        for (int i = 0; i < DISCOVERY_REPLY_COPIES; i++) {
            if (gap_ms[i]) {
                vTaskDelay(pdMS_TO_TICKS(gap_ms[i]));
            }
            send_enc(&r.key, r.resp, r.len, &r.to);
        }
        ESP_LOGI(TAG, "discovery from %s (%d replies)", inet_ntoa(r.to.sin_addr),
                 DISCOVERY_REPLY_COPIES);
    }
}

/** @brief req_set_key: store the new project key + IV and answer under the new key. */
static void on_set_key(const uint8_t *tail, size_t tail_len, const uint8_t *buf,
                       const clu_cipher_t *used, const struct sockaddr_in *from)
{
    if (tail_len < 16) {
        send_str(used, "resp:ERROR", from);
        return;
    }
    (void)used;
    memcpy(g_clu.project.key, tail, 16);
    memcpy(g_clu.project.iv, buf + 33, 16);
    g_clu.has_temp = false;
    clu_state_save_keys();
    // OM sends set_key under the old (or PIN) key but decrypts the answer with the NEW key:
    // an old-key "resp:OK" fails there with "pad block corrupted".
    send_str(&g_clu.project, "resp:OK", from);
    ESP_LOGI(TAG, "project key set by %s", inet_ntoa(from->sin_addr));
}

/** @brief `req_set_clu_ip:<sn_hex>:<ip>:<gw>`: persist a static IP, answer, then apply it. */
static void on_set_ip(const char *s, const clu_cipher_t *used, bool bcast,
                      const struct sockaddr_in *from)
{
    // req_set_clu_ip:<sn_hex>:<ip>:<gw>
    unsigned long sn = 0;
    char ip[16] = {0}, gw[16] = {0};
    int n = sscanf(s, "req_set_clu_ip:%lx:%15[^:]:%15s", &sn, ip, gw);
    if (n < 2 || sn != g_clu.sn) {
        if (!bcast) {
            send_str(used, "resp:ERROR", from);
        }
        return;
    }
    char r[64];
    if (strcmp(ip, s_ip) == 0) {
        snprintf(r, sizeof r, "resp_set_clu_ip:%08lx:%s", sn, s_ip);
        send_str(used, r, from);
        return;
    }
    if (!clu_net_set_static(ip, gw)) {
        send_str(used, "resp:ERROR", from);
        return;
    }
    // Answer from the old address, then switch (udp socket is bound to INADDR_ANY).
    snprintf(r, sizeof r, "resp_set_clu_ip:%08lx:%s", sn, ip);
    send_str(used, r, from);
    ESP_LOGW(TAG, "IP change %s -> %s (gw %s) requested by %s", s_ip, ip, gw, inet_ntoa(from->sin_addr));
    vTaskDelay(pdMS_TO_TICKS(100));
    clu_net_apply();
}

#define LUA_CALL_TIMEOUT_MS 3000

void udp_server_send_report(const char *addr, int port, const char *body)
{
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (s_sock < 0 || inet_aton(addr, &to.sin_addr) == 0) {
        return;
    }
    char *msg = malloc(strlen(body) + 32);
    if (!msg) {
        return;
    }
    sprintf(msg, "resp:0.0.0.0:00000000:%s", body);
    send_str(&g_clu.project, msg, &to);
    free(msg);
}

/**
 * @brief `req:<ip>:<session>:<script>`: evaluate the script in the Lua runtime and answer
 * `resp:<ip>:<session>:<value>`.
 *
 * @param s          Plaintext request (modified in place).
 * @param used       Key the request decrypted with (the answer uses it).
 * @param privileged Request came under the project/temp key; otherwise only checkAlive() is allowed.
 * @param from       Sender.
 */
static void on_lua(char *s, const clu_cipher_t *used, bool privileged,
                   const struct sockaddr_in *from)
{
    // req:<ip>:<session>:<script>[\r\n]
    char *p1 = strchr(s + 4, ':');
    if (!p1) {
        return;
    }
    char *p2 = strchr(p1 + 1, ':');
    if (!p2) {
        return;
    }
    *p2 = 0;
    unsigned long session = strtoul(p1 + 1, NULL, 16);
    char *script = p2 + 1;
    size_t sl = strlen(script);
    while (sl && (script[sl - 1] == '\r' || script[sl - 1] == '\n')) {
        script[--sl] = 0;
    }

    if (!privileged && strcasecmp(script, "checkAlive()") != 0) {
        send_str(used, "resp:ERROR", from);
        return;
    }
    static char val[BUF_SZ - 64];
    char from_ip[16];
    inet_ntoa_r(from->sin_addr, from_ip, sizeof from_ip);
    if (clu_lua_call(script, from_ip, val, sizeof val, LUA_CALL_TIMEOUT_MS) != ESP_OK) {
        if (strcasecmp(script, "checkAlive()") == 0) {
            strcpy(val, "false"); // runtime busy / not ready
        } else {
            ESP_LOGW(TAG, "lua call timed out: %s", script);
            strcpy(val, "nil");
        }
    }
    static char r[BUF_SZ];
    snprintf(r, sizeof r, "resp:%s:%08lx:%s", s_ip, session, val);
    send_str(used, r, from);
}

/**
 * @brief Route a decrypted packet to its handler.
 *
 * @param buf          Plaintext.
 * @param len          Plaintext length.
 * @param used         Key the packet decrypted with (answers use it).
 * @param used_project Decrypted with the project key.
 * @param used_temp    Decrypted with the temp / PIN key.
 * @param bcast        Decrypted with the broadcast key.
 * @param from         Sender.
 */
static void dispatch(uint8_t *buf, size_t len, const clu_cipher_t *used, bool used_project,
                     bool used_temp, bool bcast, const struct sockaddr_in *from)
{
    const uint8_t *tail;
    size_t tail_len;

    const bool setup = setup_window_open();

    if (keyed_cmd(buf, len, "req_discovery_clu", &tail, &tail_len)) {
        // The PIN goes to the UART console only: written to stderr directly, not through ESP_LOG,
        // so the telnet log hook (esp_log_set_vprintf) never sees it.
        fprintf(stderr, "I (%lu) %s: discovery from %s: PIN %s (UART only)\n",
                (unsigned long)esp_log_timestamp(), TAG, inet_ntoa(from->sin_addr), g_clu.pin);
        if (!setup) {
            log_locked("discovery", from); // no answer: the CLU stays invisible
            return;
        }
        on_discovery(buf, from);
        return;
    }
    buf[len] = 0; // text commands
    char *s = (char *)buf;

    if (starts(buf, len, "req_set_clu_ip")) {
        if (!setup) {
            log_locked("req_set_clu_ip", from);
            if (!bcast) {
                send_str(used, "resp:ERROR", from);
            }
            return;
        }
        on_set_ip(s, used, bcast, from);
        return;
    }
    if (bcast) {
        return;
    }
    if (keyed_cmd(buf, len, "req_set_key", &tail, &tail_len)) {
        if (!setup) {
            log_locked("req_set_key", from);
            send_str(used, "resp:ERROR", from);
            return;
        }
        on_set_key(tail, tail_len, buf, used, from);
        return;
    }
    if (starts(buf, len, "req_reset")) {
        char r[48];
        snprintf(r, sizeof r, "resp_reset:%s", s_ip);
        send_str(&g_clu.project, r, from);
        ESP_LOGW(TAG, "reset requested");
#if CONFIG_CLU_SOFT_RESET
        // Blocks this task until the old VM is gone (bus stop included); requests meanwhile wait
        // in the socket, as they would during a reboot.
        if (clu_lua_reload(10000)) {
            return;
        }
        ESP_LOGE(TAG, "soft reset failed, rebooting");
#endif
        clu_lua_halt(3000); // flush pending storage writes first
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    if (starts(buf, len, "req")) {
        on_lua(s, used, used_project || used_temp, from);
        return;
    }
    if (used_project && !setup &&
        (strcmp(s, "req_start_ftp") == 0 || starts(buf, len, "req_diagnostic_refresh"))) {
        log_locked("TFTP", from); // a session opened inside the window keeps running
        send_str(used, "resp:ERROR", from);
        return;
    }
    if (used_project && strcmp(s, "req_start_ftp") == 0) {
        clu_tftp_enable(true, from->sin_addr.s_addr); // only this host may transfer files
        send_str(used, "resp:OK", from);
        return;
    }
    // req_diagnostic_refresh:<ip>:<session> -> resp_diagnostic_refresh:<session>:OK|ERROR:<n>,
    // then OM fetches m:\DIAGNOSTIC_PACK.JSON over TFTP.
    if (used_project && starts(buf, len, "req_diagnostic_refresh")) {
        const char *sess = strrchr(s, ':') + 1; // starts() guarantees a ':'
        if (strlen(sess) > 16) return;
        int err = clu_diag_write(s_ip);
        char r[80], code[12] = "OK";
        if (err) snprintf(code, sizeof code, "ERROR:%d", err);
        snprintf(r, sizeof r, "resp_diagnostic_refresh:%.*s:%s", (int)strcspn(sess, "\r\n"), sess,
                 code);
        if (!err) clu_tftp_enable(true, from->sin_addr.s_addr); // OM downloads right after
        send_str(used, r, from);
        return;
    }
    // OM "Clear CLU": req_refresh_modules, then it downloads a:\CONFIG.JSON and imports
    // tfbusDevices (OM waits 30 s and only checks the "resp_refresh_modules" prefix).
    if (used_project && strncmp(s, "req_refresh_modules", 19) == 0) {
        bool done = tfbus_master_refresh(25000); // bus task rewrites CONFIG.JSON when done
        if (!done) {
            ESP_LOGW(TAG, "module refresh timed out");
        }
        char r[48];
        snprintf(r, sizeof r, "resp_refresh_modules:%s", s_ip);
        send_str(used, r, from);
        return;
    }
    if (used_project && strcmp(s, "req_tftp_stop") == 0) {
        clu_tftp_enable(false, 0);
        send_str(used, "resp:OK", from);
        return;
    }
    // Answer only plaintext that looks like a command. Replying to any packet that merely decrypts
    // with valid padding would make this a CBC padding oracle (decrypting captured traffic).
    if (strncmp(s, "req", 3) == 0) {
        ESP_LOGW(TAG, "unhandled from %s: %.48s", inet_ntoa(from->sin_addr), s);
        send_str(used, "resp:ERROR", from);
    }
}

/**
 * @brief Task: receive on UDP/1234, try each known key and dispatch the plaintext.
 *
 * Keys tried in order: broadcast, temp, PIN (default_key(PIN) + temp IV), project. Temp and PIN
 * only inside the setup window.
 */
static void udp_task(void *arg)
{
    static uint8_t in[BUF_SZ];
    static uint8_t plain[BUF_SZ + 1];

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(CLU_UDP_PORT),
                            .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s_sock, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "bind failed");
        close(s_sock);
        s_sock = -1;
        vTaskDelete(NULL);
        return;
    }
    // IP_PKTINFO would tell broadcast vs unicast exactly; we try keys in vCLU order instead:
    // broadcast key first marks the packet as broadcast-class.
    ESP_LOGI(TAG, "listening on udp/%d", CLU_UDP_PORT);

    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s_sock, in, sizeof in, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            continue;
        }
        // PIN key: default_key(PIN) with the temp IV from our discovery reply. OM uses it to
        // (re)key a CLU whose current project key it does not know ("set project key" with the
        // PIN from the label): req_set_clu_ip / req_set_key arrive under this key even after
        // the CLU already holds another project key. Same key as g_clu.temp (see on_discovery).
        // Outside the setup window only the broadcast and project keys are tried: the PIN key
        // (printed on the label, IV sent in clear by discovery) no longer grants anything.
        static clu_cipher_t pin_key;
        const bool temp = g_clu.has_temp && setup_window_open();
        if (temp) {
            clu_default_key((const uint8_t *)g_clu.pin, pin_key.key);
            memcpy(pin_key.iv, g_clu.temp.iv, 16);
        }
        const clu_cipher_t *keys[4] = {broadcast_key(), temp ? &g_clu.temp : NULL,
                                       temp ? &pin_key : NULL, &g_clu.project};
        bool handled = false;
        for (int i = 0; i < 4; i++) {
            if (!keys[i]) {
                continue;
            }
            int pl = clu_decrypt(keys[i], in, n, plain, BUF_SZ);
            if (pl <= 0) {
                continue;
            }
            clu_cipher_t used = *keys[i];
            dispatch(plain, pl, &used, i == 3, i == 1 || i == 2, i == 0, &from);
            handled = true;
            break;
        }
        if (!handled) {
            ESP_LOGW(TAG, "%d bytes from %s: no key decrypts it (wrong project key / PIN?)", n,
                     inet_ntoa(from.sin_addr));
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, in, n > 64 ? 64 : n, ESP_LOG_WARN);
            if (g_clu.has_temp) {
                ESP_LOG_BUFFER_HEX_LEVEL("udp temp iv", g_clu.temp.iv, 16, ESP_LOG_WARN);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Outgoing Lua calls to other CLUs (OM.LUA: CLUxxx:execute(0, "script")). All CLUs of a project
// share the project key. Each call uses its own ephemeral socket; the remote CLU answers to the
// source port with resp:<ip>:<session>:<result>.
// ---------------------------------------------------------------------------
#define REMOTE_QUEUE      16
#define REMOTE_TIMEOUT_MS 1500

/** @brief Queued outgoing remote-CLU call. */
typedef struct {
    uint32_t ip; // host order
    uint32_t id; // clu_lua call id
    char *script;
} remote_req_t;

static QueueHandle_t s_remote_q;

uint32_t udp_server_remote_exec(uint32_t ip, const char *script)
{
    static uint32_t s_next_id; // Lua task only
    if (!s_remote_q) {
        return 0;
    }
    if (++s_next_id == 0) {
        s_next_id = 1;
    }
    remote_req_t r = {.ip = ip, .id = s_next_id, .script = strdup(script)};
    if (!r.script) {
        return 0;
    }
    if (xQueueSend(s_remote_q, &r, 0) != pdTRUE) {
        ESP_LOGW(TAG, "remote queue full, dropped: %s", script);
        free(r.script);
        return 0;
    }
    return r.id;
}

/**
 * @brief Send one remote call (`req:<ip>:<session>:<script>`) and post the matching resp to clu_lua.
 */
static void remote_call(int sock, const remote_req_t *r)
{
    static char msg[BUF_SZ];
    static uint8_t enc[BUF_SZ + 16];
    static uint8_t in[BUF_SZ];
    static char plain[BUF_SZ + 1];

    uint32_t session;
    clu_random((uint8_t *)&session, sizeof session);
    int ml = snprintf(msg, sizeof msg, "req:%s:%08lx:%s", s_ip, (unsigned long)session, r->script);
    if (ml <= 0 || ml >= (int)sizeof msg) {
        ESP_LOGW(TAG, "remote script too long");
        clu_lua_post_remote(r->id, NULL);
        return;
    }
    clu_cipher_t key = g_clu.project;
    int el = clu_encrypt(&key, (const uint8_t *)msg, ml, enc, sizeof enc);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(CLU_UDP_PORT),
                             .sin_addr.s_addr = htonl(r->ip)};
    char ip[16];
    inet_ntoa_r(to.sin_addr, ip, sizeof ip);
    if (el <= 0 || sendto(sock, enc, el, 0, (struct sockaddr *)&to, sizeof to) < 0) {
        ESP_LOGW(TAG, "remote %s: send failed", ip);
        clu_lua_post_remote(r->id, NULL);
        return;
    }

    char sess[10];
    snprintf(sess, sizeof sess, "%08lx", (unsigned long)session);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(REMOTE_TIMEOUT_MS);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(sock, in, sizeof in, 0, (struct sockaddr *)&from, &fl);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue; // SO_RCVTIMEO slice: wait until the deadline
        }
        if (n <= 0) {
            break;
        }
        if (from.sin_addr.s_addr != to.sin_addr.s_addr) {
            continue;
        }
        int pl = clu_decrypt(&key, in, n, (uint8_t *)plain, BUF_SZ);
        if (pl <= 0) {
            continue;
        }
        plain[pl] = 0;
        // resp:<ip>:<session>:<result>
        char *p1 = strncmp(plain, "resp:", 5) == 0 ? strchr(plain + 5, ':') : NULL;
        if (!p1 || strncasecmp(p1 + 1, sess, 8) != 0 || p1[9] != ':') {
            continue;
        }
        ESP_LOGD(TAG, "remote %s: %s -> %s", ip, r->script, p1 + 10);
        clu_lua_post_remote(r->id, p1 + 10);
        return;
    }
    ESP_LOGW(TAG, "remote %s: no response to %s", ip, r->script);
    clu_lua_post_remote(r->id, NULL);
}

/** @brief Task: execute queued remote-CLU calls one at a time on an ephemeral socket. */
static void remote_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "remote: socket/bind failed, remote CLU calls disabled");
        if (sock >= 0) {
            close(sock);
        }
        for (;;) { // keep draining so queued scripts are freed (the queue stays valid for callers)
            remote_req_t r;
            xQueueReceive(s_remote_q, &r, portMAX_DELAY);
            clu_lua_post_remote(r.id, NULL);
            free(r.script);
        }
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200 * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (;;) {
        remote_req_t r;
        xQueueReceive(s_remote_q, &r, portMAX_DELAY);
        remote_call(sock, &r);
        free(r.script);
    }
}

void udp_server_start(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    s_remote_q = xQueueCreate(REMOTE_QUEUE, sizeof(remote_req_t));
    s_discovery_q = xQueueCreate(DISCOVERY_QUEUE, sizeof(discovery_reply_t));
    xTaskCreatePinnedToCore(discovery_task, "clu_disc", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(udp_task, "clu_udp", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(remote_task, "clu_remote", 6144, NULL, 4, NULL, 0);
    setup_start();
}
