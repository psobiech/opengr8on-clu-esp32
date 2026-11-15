/**
 * @file clu_sec_engine.c
 * @brief Secure CLU channel engine (see clu_sec_engine.h).
 */
#include "clu_sec_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t PROLOGUE[] = "opengr8on-sec-v1";

#define PROLOGUE_LEN (sizeof PROLOGUE - 1)

#define HS_PAYLOAD_LEN 14
#define HS_PAYLOAD_MAX 24

static bool release_ip(sec_pins_t *pins, uint32_t peer_ip, const sec_pin_t *keep, bool changed);
static void on_probe(sec_engine_t *engine, uint32_t peer_ip, uint16_t port, const uint8_t *buf, size_t len);
static void on_hs_init(sec_engine_t *engine, uint32_t peer_ip, uint16_t port, const uint8_t *buf, size_t len, uint32_t now_ms);
static sec_session_t *alloc_session(sec_engine_t *engine);
static int hs_payload(sec_engine_t *engine, uint8_t out[HS_PAYLOAD_MAX], bool initiator);
static void put32(uint8_t *buffer, uint32_t value);
static void put64(uint8_t *buffer, uint64_t value);
static bool hs_fresh(sec_engine_t *engine, const uint8_t pub[NOISE_KEY_LEN], uint64_t timestamp_ms, uint32_t now_ms, bool commit);
static bool hs_budget(sec_engine_t *engine, uint32_t now_ms);
static void on_hs_resp(sec_engine_t *engine, uint32_t peer_ip, const uint8_t *buf, size_t len, uint32_t now_ms);
static bool parse_payload(const uint8_t *payload_bytes, int len, uint32_t *serial, uint8_t *flags, uint64_t *timestamp_ms);
static void on_data(sec_engine_t *engine, uint32_t peer_ip, uint16_t port, const uint8_t *buf, size_t len, uint32_t now_ms);
static uint64_t get64(const uint8_t *buffer);
static uint32_t get32(const uint8_t *buffer);
static int32_t ms_since(uint32_t now, uint32_t then);
static void free_session(sec_session_t *session);
static sec_session_t *by_local_idx(sec_engine_t *engine, uint32_t idx);

bool sec_replay_check(sec_replay_t *replay, uint64_t counter)
{
    if (!replay->any) {
        replay->any = true;
        replay->top = counter;
        replay->bitmap = 1;
        return true;
    }
    if (counter > replay->top) {
        uint64_t shift = counter - replay->top;
        replay->bitmap = shift >= 64 ? 0 : replay->bitmap << shift;
        replay->bitmap |= 1;
        replay->top = counter;
        return true;
    }
    uint64_t back = replay->top - counter;
    if (back >= 64) {
        return false;
    }
    uint64_t bit = 1ull << back;
    if (replay->bitmap & bit) {
        return false;
    }
    replay->bitmap |= bit;
    return true;
}

sec_pin_t *sec_pins_by_pub(sec_pins_t *pins, const uint8_t pub[NOISE_KEY_LEN])
{
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_PINS; i++) {
        if (pins->entries[i].flags && memcmp(pins->entries[i].pub, pub, NOISE_KEY_LEN) == 0) {
            return &pins->entries[i];
        }
    }
    return NULL;
}

sec_pin_t *sec_pins_by_sn(sec_pins_t *pins, uint32_t serial)
{
    for (int i = 0; serial && i < CONFIG_CLU_SEC_MAX_PINS; i++) {
        if (pins->entries[i].flags && pins->entries[i].serial == serial) {
            return &pins->entries[i];
        }
    }
    return NULL;
}

sec_pin_t *sec_pins_by_ip(sec_pins_t *pins, uint32_t peer_ip)
{
    for (int i = 0; peer_ip && i < CONFIG_CLU_SEC_MAX_PINS; i++) {
        if (pins->entries[i].flags && pins->entries[i].peer_ip == peer_ip) {
            return &pins->entries[i];
        }
    }
    return NULL;
}

sec_pin_result_t sec_pins_check(sec_pins_t *pins, const uint8_t pub[NOISE_KEY_LEN], uint32_t serial, uint32_t peer_ip,
                                uint8_t flags, bool allow_new, bool *changed)
{
    *changed = false;
    sec_pin_t *pin = sec_pins_by_pub(pins, pub);
    if (pin) {
        if (serial && pin->serial != serial) {
            sec_pin_t *other = sec_pins_by_sn(pins, serial);
            if (other && other != pin) {
                return SEC_PIN_CONFLICT;
            }
            pin->serial = serial;
            *changed = true;
        }
        if (peer_ip && pin->peer_ip != peer_ip) {
            pin->peer_ip = peer_ip;
            *changed = true;
        }
        *changed = release_ip(pins, peer_ip, pin, *changed);
        return SEC_PIN_OK;
    }
    if (sec_pins_by_sn(pins, serial)) {
        return SEC_PIN_CONFLICT;
    }
    if (!allow_new) {
        return SEC_PIN_UNKNOWN;
    }
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_PINS; i++) {
        if (!pins->entries[i].flags) {
            pin = &pins->entries[i];
            memset(pin, 0, sizeof *pin);
            memcpy(pin->pub, pub, NOISE_KEY_LEN);
            pin->serial = serial;
            pin->peer_ip = peer_ip;
            pin->flags = flags ? flags : SEC_PIN_CLU;
            release_ip(pins, peer_ip, pin, true);
            *changed = true;
            return SEC_PIN_ADDED;
        }
    }
    return SEC_PIN_FULL;
}

void sec_fingerprint(const uint8_t pub[NOISE_KEY_LEN], char out[17])
{
    uint8_t hash[NOISE_KEY_LEN];
    if (noise_sha256(pub, NOISE_KEY_LEN, hash)) {
        memset(hash, 0, sizeof hash);
    }
    for (int i = 0; i < 8; i++) {
        snprintf(out + 2 * i, 3, "%02x", hash[i]);
    }
}

int sec_pins_forget(sec_pins_t *pins, const char *what)
{
    size_t len = strlen(what);
    char *end = NULL;
    unsigned long serial_number = strtoul(what, &end, 10);
    bool by_sn = len && *end == 0 && serial_number != 0;
    int removed = 0;
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_PINS; i++) {
        if (!pins->entries[i].flags) {
            continue;
        }
        bool match = by_sn && pins->entries[i].serial == (uint32_t)serial_number;
        if (!match && len >= 8 && len <= 16) {
            char fingerprint_text[17];
            sec_fingerprint(pins->entries[i].pub, fingerprint_text);
            match = strncmp(fingerprint_text, what, len) == 0;
        }
        if (match) {
            memset(&pins->entries[i], 0, sizeof pins->entries[i]);
            removed++;
        }
    }
    return removed;
}

void sec_engine_init(sec_engine_t *engine, const noise_keypair_t *local_key, uint32_t serial, const sec_ops_t *ops)
{
    memset(engine, 0, sizeof *engine);
    engine->local_static = *local_key;
    engine->serial = serial;
    engine->ops = *ops;
}

void sec_engine_input(sec_engine_t *engine, uint32_t peer_ip, uint16_t port, const uint8_t *buf, size_t len, uint32_t now_ms)
{
    if (len < 1) {
        return;
    }
    switch (buf[0]) {
    case SEC_T_PROBE:
        on_probe(engine, peer_ip, port, buf, len);
        break;
    case SEC_T_HS_INIT:
        on_hs_init(engine, peer_ip, port, buf, len, now_ms);
        break;
    case SEC_T_HS_RESP:
        on_hs_resp(engine, peer_ip, buf, len, now_ms);
        break;
    case SEC_T_DATA:
        on_data(engine, peer_ip, port, buf, len, now_ms);
        break;
    default:
        break; // PROBE_RESP is read by the prober's own socket
    }
}

size_t sec_probe_build(uint32_t nonce, uint8_t out[SEC_PROBE_LEN])
{
    memset(out, 0, SEC_PROBE_LEN);
    out[0] = SEC_T_PROBE;
    put32(out + 1, nonce);
    return SEC_PROBE_LEN;
}

bool sec_probe_parse(const uint8_t *buf, size_t len, uint32_t nonce, uint32_t *serial, uint8_t pub[NOISE_KEY_LEN])
{
    if (len < SEC_PROBE_RESP_LEN || buf[0] != SEC_T_PROBE_RESP || get32(buf + 1) != nonce || buf[5] != SEC_VERSION) {
        return false;
    }
    *serial = get32(buf + 6);
    memcpy(pub, buf + 10, NOISE_KEY_LEN);
    return true;
}

int sec_engine_connect(sec_engine_t *engine, uint32_t peer_ip, uint16_t port, const uint8_t peer_pub[NOISE_KEY_LEN],
                       uint32_t now_ms)
{
    // One pending handshake per peer: a new attempt replaces the old one.
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_SESSIONS; i++) {
        if (engine->sess[i].state == SEC_S_INIT_SENT && engine->sess[i].peer_ip == peer_ip) {
            free_session(&engine->sess[i]);
        }
    }
    sec_session_t *session = alloc_session(engine);
    if (!session) {
        return -1;
    }
    noise_ik_init(&session->handshake, true, PROLOGUE, PROLOGUE_LEN, &engine->local_static, peer_pub);
    uint8_t payload[HS_PAYLOAD_MAX];
    int plen = hs_payload(engine, payload, true);
    uint8_t out[5 + NOISE_IK_MSG1_LEN + HS_PAYLOAD_MAX];
    out[0] = SEC_T_HS_INIT;
    put32(out + 1, session->local_idx);
    int mlen = noise_ik_write_msg1(&session->handshake, payload, (size_t)plen, out + 5, sizeof out - 5);
    if (mlen < 0) {
        free_session(session);
        return -1;
    }
    session->state = SEC_S_INIT_SENT;
    session->initiator = true;
    session->peer_ip = peer_ip;
    session->port = port;
    memcpy(session->peer_pub, peer_pub, NOISE_KEY_LEN);
    session->created_ms = session->last_ms = now_ms;
    session->hs_len = (uint16_t)(5 + mlen);
    memcpy(session->hs_msg, out, session->hs_len);
    engine->ops.send(engine->ops.ctx, peer_ip, port, out, session->hs_len);
    return 0;
}

int sec_engine_retransmit(sec_engine_t *engine, uint32_t peer_ip)
{
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_SESSIONS; i++) {
        sec_session_t *session = &engine->sess[i];
        if (session->state == SEC_S_INIT_SENT && session->peer_ip == peer_ip) {
            engine->ops.send(engine->ops.ctx, session->peer_ip, session->port, session->hs_msg, session->hs_len);
            return 0;
        }
    }
    return -1;
}

const sec_session_t *sec_engine_session_for(sec_engine_t *engine, uint32_t peer_ip, uint32_t now_ms, bool *rekey)
{
    const sec_session_t *best = NULL;
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_SESSIONS; i++) {
        const sec_session_t *session = &engine->sess[i];
        if (session->state != SEC_S_ESTABLISHED || !session->confirmed || session->peer_ip != peer_ip ||
            ms_since(now_ms, session->created_ms) > (int32_t)SEC_REJECT_AFTER_MS || session->send_n >= SEC_REKEY_AFTER_MSGS * 2) {
            continue;
        }
        if (!best || ms_since(session->created_ms, best->created_ms) > 0) {
            best = session; // newest
        }
    }
    if (rekey) {
        *rekey = !best || (best->initiator && (ms_since(now_ms, best->created_ms) > (int32_t)SEC_REKEY_AFTER_MS ||
                                               best->send_n >= SEC_REKEY_AFTER_MSGS));
    }
    return best;
}

bool sec_engine_pending(const sec_engine_t *engine, uint32_t peer_ip)
{
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_SESSIONS; i++) {
        if (engine->sess[i].state == SEC_S_INIT_SENT && engine->sess[i].peer_ip == peer_ip) {
            return true;
        }
    }
    return false;
}

int sec_engine_send(sec_engine_t *engine, uint32_t handle, const uint8_t *plain, size_t len, uint32_t now_ms)
{
    static uint8_t out[1500 + SEC_DATA_OVERHEAD];
    sec_session_t *session = by_local_idx(engine, handle);
    if (!session || session->state != SEC_S_ESTABLISHED || len > sizeof out - SEC_DATA_OVERHEAD ||
        ms_since(now_ms, session->created_ms) > (int32_t)SEC_REJECT_AFTER_MS) {
        return -1;
    }
    out[0] = SEC_T_DATA;
    put32(out + 1, session->remote_idx);
    put64(out + 5, session->send_n);
    int clen = noise_aead_encrypt(session->send_k, session->send_n, out, SEC_DATA_HDR, plain, len, out + SEC_DATA_HDR,
                                  sizeof out - SEC_DATA_HDR);
    if (clen < 0) {
        return -1;
    }
    session->send_n++;
    session->last_ms = now_ms;
    engine->ops.send(engine->ops.ctx, session->peer_ip, session->port, out, SEC_DATA_HDR + (size_t)clen);
    return 0;
}

void sec_engine_tick(sec_engine_t *engine, uint32_t now_ms)
{
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_SESSIONS; i++) {
        sec_session_t *session = &engine->sess[i];
        if (session->state == SEC_S_INIT_SENT && ms_since(now_ms, session->created_ms) > (int32_t)SEC_HS_TIMEOUT_MS) {
            uint32_t peer_ip = session->peer_ip;
            free_session(session);
            if (engine->ops.handshake_done) {
                engine->ops.handshake_done(engine->ops.ctx, peer_ip, false);
            }
        } else if (session->state == SEC_S_ESTABLISHED && (ms_since(now_ms, session->last_ms) > (int32_t)SEC_IDLE_MS ||
                                                     ms_since(now_ms, session->created_ms) > (int32_t)SEC_REJECT_AFTER_MS)) {
            free_session(session);
        }
    }
}

int sec_engine_session_count(const sec_engine_t *engine)
{
    int established_count = 0;
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_SESSIONS; i++) {
        established_count += engine->sess[i].state == SEC_S_ESTABLISHED;
    }
    return established_count;
}

/** @brief Clear @p peer_ip from every pin except @p keep (an address now belongs to another peer). */
static bool release_ip(sec_pins_t *pins, uint32_t peer_ip, const sec_pin_t *keep, bool changed)
{
    for (int i = 0; peer_ip && i < CONFIG_CLU_SEC_MAX_PINS; i++) {
        if (&pins->entries[i] != keep && pins->entries[i].flags && pins->entries[i].peer_ip == peer_ip) {
            pins->entries[i].peer_ip = 0;
            changed = true;
        }
    }
    return changed;
}

static void on_probe(sec_engine_t *engine, uint32_t peer_ip, uint16_t port, const uint8_t *buf, size_t len)
{
    if (len < SEC_PROBE_LEN) {
        return; // a short probe would turn this into a reflector
    }
    uint8_t out[SEC_PROBE_RESP_LEN];
    out[0] = SEC_T_PROBE_RESP;
    memcpy(out + 1, buf + 1, 4);
    out[5] = SEC_VERSION;
    put32(out + 6, engine->serial);
    memcpy(out + 10, engine->local_static.pub, NOISE_KEY_LEN);
    engine->ops.send(engine->ops.ctx, peer_ip, port, out, sizeof out);
}

static void on_hs_init(sec_engine_t *engine, uint32_t peer_ip, uint16_t port, const uint8_t *buf, size_t len, uint32_t now_ms)
{
    if (len < 5 + NOISE_IK_MSG1_LEN || !hs_budget(engine, now_ms)) {
        return;
    }
    noise_hs_t handshake;
    noise_ik_init(&handshake, false, PROLOGUE, PROLOGUE_LEN, &engine->local_static, NULL);
    uint8_t payload[HS_PAYLOAD_MAX];
    int plen = noise_ik_read_msg1(&handshake, buf + 5, len - 5, payload, sizeof payload);
    uint32_t serial = 0;
    uint8_t flags = 0;
    uint64_t timestamp_ms = 0;
    if (plen < 0 || !parse_payload(payload, plen, &serial, &flags, &timestamp_ms) || !hs_fresh(engine, handshake.remote_static, timestamp_ms, now_ms, false)) {
        noise_wipe(&handshake, sizeof handshake);
        return;
    }
    bool changed = false;
    bool allow = engine->ops.pairing_allowed && engine->ops.pairing_allowed(engine->ops.ctx);
    // Known keys only refresh peer_ip / serial here; a new key is pinned on the first DATA (on_data).
    sec_pin_result_t pin_result = sec_pins_check(&engine->pins, handshake.remote_static, serial, peer_ip, flags, false, &changed);
    if (changed && engine->ops.pins_changed) {
        engine->ops.pins_changed(engine->ops.ctx);
    }
    if (pin_result != SEC_PIN_OK && !(pin_result == SEC_PIN_UNKNOWN && allow)) {
        noise_wipe(&handshake, sizeof handshake); // silently: an unknown key learns nothing
        return;
    }
    hs_fresh(engine, handshake.remote_static, timestamp_ms, now_ms, true);
    sec_session_t *session = alloc_session(engine);
    if (!session) {
        noise_wipe(&handshake, sizeof handshake);
        return;
    }
    uint8_t out[9 + NOISE_IK_MSG2_LEN + HS_PAYLOAD_MAX];
    out[0] = SEC_T_HS_RESP;
    put32(out + 1, session->local_idx);
    memcpy(out + 5, buf + 1, 4);
    int rplen = hs_payload(engine, payload, false);
    int mlen = noise_ik_write_msg2(&handshake, payload, (size_t)rplen, out + 9, sizeof out - 9);
    if (mlen < 0) {
        noise_wipe(&handshake, sizeof handshake);
        free_session(session);
        return;
    }
    session->state = SEC_S_ESTABLISHED;
    session->initiator = false;
    session->remote_idx = get32(buf + 1);
    session->peer_ip = peer_ip;
    session->port = port;
    session->peer_sn = serial;
    session->peer_flags = flags;
    session->pin_pending = pin_result == SEC_PIN_UNKNOWN;
    memcpy(session->peer_pub, handshake.remote_static, NOISE_KEY_LEN);
    session->created_ms = session->last_ms = now_ms;
    noise_split(&handshake, session->send_k, session->recv_k);
    engine->ops.send(engine->ops.ctx, peer_ip, port, out, 9 + (size_t)mlen);
}

/**
 * @brief Free slot for a new session; when full, evict the least recently used established one
 * (never a pending handshake).
 */
static sec_session_t *alloc_session(sec_engine_t *engine)
{
    sec_session_t *lru = NULL;
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_SESSIONS; i++) {
        sec_session_t *session = &engine->sess[i];
        if (session->state == SEC_S_FREE) {
            lru = session;
            break;
        }
        if (session->state == SEC_S_ESTABLISHED && (!lru || ms_since(lru->last_ms, session->last_ms) > 0)) {
            lru = session;
        }
    }
    if (!lru) {
        return NULL;
    }
    free_session(lru);
    do {
        engine->ops.random(engine->ops.ctx, (uint8_t *)&lru->local_idx, sizeof lru->local_idx);
    } while (lru->local_idx == 0 || by_local_idx(engine, lru->local_idx));
    return lru;
}

/**
 * @brief Handshake payload: version(1) kind(1: 'c' CLU / 'h' host) serial(4) timestamp_ms(8), big endian.
 *
 * timestamp_ms is the initiator's wall clock in unix ms (0 = no valid clock); strictly increasing per
 * initiator so that a recorded HS_INIT cannot be replayed (see hs_fresh()). The responder sends 0.
 */
static int hs_payload(sec_engine_t *engine, uint8_t out[HS_PAYLOAD_MAX], bool initiator)
{
    uint64_t timestamp_ms = 0;
    if (initiator && engine->ops.wall_ms) {
        timestamp_ms = engine->ops.wall_ms(engine->ops.ctx);
        if (timestamp_ms) {
            timestamp_ms = timestamp_ms > engine->last_ts ? timestamp_ms : engine->last_ts + 1;
            engine->last_ts = timestamp_ms;
        }
    }
    out[0] = SEC_VERSION;
    out[1] = engine->serial ? 'c' : 'h';
    put32(out + 2, engine->serial);
    put64(out + 6, timestamp_ms);
    return HS_PAYLOAD_LEN;
}

static void put32(uint8_t *buffer, uint32_t value)
{
    for (int i = 0; i < 4; i++) {
        buffer[i] = (uint8_t)(value >> (8 * i));
    }
}

static void put64(uint8_t *buffer, uint64_t value)
{
    for (int i = 0; i < 8; i++) {
        buffer[i] = (uint8_t)(value >> (8 * i));
    }
}

/**
 * @brief HS_INIT replay check for initiator @p pub carrying @p timestamp_ms.
 *
 * timestamp_ms == 0 (initiator without a clock) is not protected. Otherwise timestamp_ms must be newer than the last
 * accepted one (the same value is tolerated for SEC_HS_TIMEOUT_MS: the unchanged retransmission),
 * and within SEC_HS_SKEW_MS of our own clock when we have one (covers our reboot losing the table).
 * Only @p commit records timestamp_ms, which is done once the peer passed the pin check (an arbitrary key
 * must not be able to evict real entries).
 */
static bool hs_fresh(sec_engine_t *engine, const uint8_t pub[NOISE_KEY_LEN], uint64_t timestamp_ms, uint32_t now_ms, bool commit)
{
    if (!timestamp_ms) {
        return true;
    }
    uint64_t wall = engine->ops.wall_ms ? engine->ops.wall_ms(engine->ops.ctx) : 0;
    if (wall && (timestamp_ms > wall ? timestamp_ms - wall : wall - timestamp_ms) > SEC_HS_SKEW_MS) {
        return false;
    }
    sec_hs_seen_t *slot = NULL, *lru = &engine->seen[0];
    for (int i = 0; i < SEC_HS_SEEN; i++) {
        sec_hs_seen_t *seen_entry = &engine->seen[i];
        if (seen_entry->used && memcmp(seen_entry->pub, pub, NOISE_KEY_LEN) == 0) {
            slot = seen_entry;
            break;
        }
        if (!seen_entry->used || (lru->used && ms_since(lru->at_ms, seen_entry->at_ms) > 0)) {
            lru = seen_entry;
        }
    }
    if (slot) {
        if (timestamp_ms < slot->timestamp_ms || (timestamp_ms == slot->timestamp_ms && ms_since(now_ms, slot->at_ms) > (int32_t)SEC_HS_TIMEOUT_MS)) {
            return false;
        }
        if (commit && timestamp_ms > slot->timestamp_ms) {
            slot->timestamp_ms = timestamp_ms;
            slot->at_ms = now_ms;
        }
        return true;
    }
    if (commit) {
        lru->used = true;
        memcpy(lru->pub, pub, NOISE_KEY_LEN);
        lru->timestamp_ms = timestamp_ms;
        lru->at_ms = now_ms;
    }
    return true;
}

/** @brief Responder handshake budget: SEC_HS_RATE_PER_S per second, burst of the same size. */
static bool hs_budget(sec_engine_t *engine, uint32_t now_ms)
{
    const uint32_t per = 1000u / SEC_HS_RATE_PER_S;
    const uint32_t burst = per * SEC_HS_RATE_PER_S;
    if (!engine->hs_bucket_init || ms_since(now_ms, engine->hs_tokens_ms) > (int32_t)burst) {
        engine->hs_tokens_ms = now_ms - burst;
        engine->hs_bucket_init = true;
    }
    if (ms_since(now_ms, engine->hs_tokens_ms) < (int32_t)per) {
        return false;
    }
    engine->hs_tokens_ms += per;
    return true;
}

static void on_hs_resp(sec_engine_t *engine, uint32_t peer_ip, const uint8_t *buf, size_t len, uint32_t now_ms)
{
    if (len < 9 + NOISE_IK_MSG2_LEN) {
        return;
    }
    sec_session_t *session = by_local_idx(engine, get32(buf + 5));
    if (!session || session->state != SEC_S_INIT_SENT || session->peer_ip != peer_ip) {
        return;
    }
    noise_hs_t handshake = session->handshake; // a forged HS_RESP must not destroy the pending handshake
    uint8_t payload[HS_PAYLOAD_MAX];
    int plen = noise_ik_read_msg2(&handshake, buf + 9, len - 9, payload, sizeof payload);
    uint32_t serial = 0;
    uint8_t flags = 0;
    uint64_t timestamp_ms = 0;
    if (plen < 0 || !parse_payload(payload, plen, &serial, &flags, &timestamp_ms)) {
        noise_wipe(&handshake, sizeof handshake);
        return;
    }
    session->remote_idx = get32(buf + 1);
    session->peer_sn = serial;
    noise_split(&handshake, session->send_k, session->recv_k);
    noise_wipe(&session->handshake, sizeof session->handshake);
    noise_wipe(session->hs_msg, sizeof session->hs_msg);
    session->hs_len = 0;
    session->state = SEC_S_ESTABLISHED;
    session->confirmed = true;
    session->created_ms = session->last_ms = now_ms;
    if (engine->ops.handshake_done) {
        engine->ops.handshake_done(engine->ops.ctx, peer_ip, true);
    }
}

/** @brief Parse a handshake payload; returns false on a malformed / unsupported one. */
static bool parse_payload(const uint8_t *payload_bytes, int len, uint32_t *serial, uint8_t *flags, uint64_t *timestamp_ms)
{
    if (len != HS_PAYLOAD_LEN || payload_bytes[0] != SEC_VERSION || (payload_bytes[1] != 'c' && payload_bytes[1] != 'h')) {
        return false;
    }
    *flags = payload_bytes[1] == 'h' ? SEC_PIN_HOST : SEC_PIN_CLU;
    *serial = get32(payload_bytes + 2);
    *timestamp_ms = get64(payload_bytes + 6);
    return true;
}

static void on_data(sec_engine_t *engine, uint32_t peer_ip, uint16_t port, const uint8_t *buf, size_t len, uint32_t now_ms)
{
    static uint8_t plain[1500];
    if (len < SEC_DATA_OVERHEAD || len - SEC_DATA_OVERHEAD > sizeof plain) {
        return;
    }
    sec_session_t *session = by_local_idx(engine, get32(buf + 1));
    if (!session || session->state != SEC_S_ESTABLISHED || ms_since(now_ms, session->created_ms) > (int32_t)SEC_REJECT_AFTER_MS) {
        return;
    }
    uint64_t counter = get64(buf + 5);
    int plen = noise_aead_decrypt(session->recv_k, counter, buf, SEC_DATA_HDR, buf + SEC_DATA_HDR, len - SEC_DATA_HDR,
                                  plain, sizeof plain);
    if (plen < 0 || !sec_replay_check(&session->replay, counter)) {
        return; // the window is only updated by authentic packets
    }
    if (session->pin_pending) {
        // Pairing was allowed when the handshake arrived; the window may have closed since.
        bool changed = false;
        sec_pin_result_t pin_result = sec_pins_check(&engine->pins, session->peer_pub, session->peer_sn, peer_ip, session->peer_flags, true, &changed);
        if (changed && engine->ops.pins_changed) {
            engine->ops.pins_changed(engine->ops.ctx);
        }
        if (pin_result != SEC_PIN_OK && pin_result != SEC_PIN_ADDED) {
            free_session(session); // SN taken by another key meanwhile, or no room
            return;
        }
        session->pin_pending = false;
    }
    session->confirmed = true;
    session->last_ms = now_ms;
    session->peer_ip = peer_ip; // roaming: the authenticated source becomes the reply address
    session->port = port;
    engine->ops.deliver(engine->ops.ctx, session->local_idx, session, plain, (size_t)plen);
}

static uint64_t get64(const uint8_t *buffer)
{
    return (uint64_t)get32(buffer) | (uint64_t)get32(buffer + 4) << 32;
}

static uint32_t get32(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] | (uint32_t)buffer[1] << 8 | (uint32_t)buffer[2] << 16 | (uint32_t)buffer[3] << 24;
}

/** @brief now - then as a signed delta (wrap-safe for uint32 milliseconds). */
static int32_t ms_since(uint32_t now, uint32_t then)
{
    return (int32_t)(now - then);
}

static void free_session(sec_session_t *session)
{
    noise_wipe(session, sizeof *session);
}

static sec_session_t *by_local_idx(sec_engine_t *engine, uint32_t idx)
{
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_SESSIONS; i++) {
        if (engine->sess[i].state != SEC_S_FREE && engine->sess[i].local_idx == idx) {
            return &engine->sess[i];
        }
    }
    return NULL;
}
