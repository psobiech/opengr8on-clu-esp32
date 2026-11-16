// Host test for components/clu_secure: Noise IK against vectors from the Python noiseprotocol
// package (fixtures/noise_ik_vectors.h), the replay window, the pin rules and two engines talking
// to each other over a fake network. Linked against the system mbedtls 3.6 PSA implementation.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clu_noise.h"
#include "clu_sec_engine.h"
#include "fixtures/noise_ik_vectors.h"
#include "psa/crypto.h"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------------------
// Noise IK interop
// ---------------------------------------------------------------------------

static void test_noise_vectors(void)
{
    noise_keypair_t is, rs, ie, re;
    check("keypair from priv", noise_keypair_from_priv(&is, V_I_S) == 0 && noise_keypair_from_priv(&rs, V_R_S) == 0 &&
                                   noise_keypair_from_priv(&ie, V_I_E) == 0 && noise_keypair_from_priv(&re, V_R_E) == 0);
    check("responder public key matches python", memcmp(rs.pub, V_R_S_PUB, 32) == 0);

    noise_hs_t ini, resp;
    noise_ik_init(&ini, true, V_PROLOGUE, sizeof V_PROLOGUE, &is, rs.pub);
    ini.local_ephemeral = ie;
    ini.has_local_ephemeral = true;
    uint8_t m1[200];
    int n1 = noise_ik_write_msg1(&ini, V_P1, sizeof V_P1, m1, sizeof m1);
    check("msg1 matches python", n1 == (int)sizeof V_MSG1 && memcmp(m1, V_MSG1, sizeof V_MSG1) == 0);

    noise_ik_init(&resp, false, V_PROLOGUE, sizeof V_PROLOGUE, &rs, NULL);
    resp.local_ephemeral = re;
    resp.has_local_ephemeral = true;
    uint8_t p[64];
    int pn = noise_ik_read_msg1(&resp, V_MSG1, sizeof V_MSG1, p, sizeof p);
    check("msg1 payload", pn == (int)sizeof V_P1 && memcmp(p, V_P1, sizeof V_P1) == 0);
    check("responder learns initiator static", memcmp(resp.remote_static, is.pub, 32) == 0);

    uint8_t m2[200];
    int n2 = noise_ik_write_msg2(&resp, V_P2, sizeof V_P2, m2, sizeof m2);
    check("msg2 matches python", n2 == (int)sizeof V_MSG2 && memcmp(m2, V_MSG2, sizeof V_MSG2) == 0);
    pn = noise_ik_read_msg2(&ini, V_MSG2, sizeof V_MSG2, p, sizeof p);
    check("msg2 payload", pn == (int)sizeof V_P2 && memcmp(p, V_P2, sizeof V_P2) == 0);

    uint8_t is_send[32], is_recv[32], rs_send[32], rs_recv[32];
    noise_split(&ini, is_send, is_recv);
    noise_split(&resp, rs_send, rs_recv);
    check("split k1 matches python", memcmp(is_send, V_K1, 32) == 0 && memcmp(rs_recv, V_K1, 32) == 0);
    check("split k2 matches python", memcmp(is_recv, V_K2, 32) == 0 && memcmp(rs_send, V_K2, 32) == 0);

    uint8_t t[128];
    int tn = noise_aead_encrypt(is_send, 0, NULL, 0, V_T1_PLAIN, sizeof V_T1_PLAIN, t, sizeof t);
    check("transport msg matches python", tn == (int)sizeof V_T1 && memcmp(t, V_T1, sizeof V_T1) == 0);

    // Tampering
    uint8_t bad[sizeof V_MSG1];
    memcpy(bad, V_MSG1, sizeof bad);
    bad[40] ^= 1; // inside enc(s)
    noise_ik_init(&resp, false, V_PROLOGUE, sizeof V_PROLOGUE, &rs, NULL);
    check("tampered msg1 rejected", noise_ik_read_msg1(&resp, bad, sizeof bad, p, sizeof p) < 0);
    noise_ik_init(&resp, false, (const uint8_t *)"other", 5, &rs, NULL);
    check("prologue mismatch rejected", noise_ik_read_msg1(&resp, V_MSG1, sizeof V_MSG1, p, sizeof p) < 0);
    noise_ik_init(&resp, false, V_PROLOGUE, sizeof V_PROLOGUE, &is, NULL); // wrong responder key
    check("msg1 for another responder rejected", noise_ik_read_msg1(&resp, V_MSG1, sizeof V_MSG1, p, sizeof p) < 0);
    check("truncated msg1 rejected", noise_ik_read_msg1(&resp, V_MSG1, NOISE_IK_MSG1_LEN - 1, p, sizeof p) < 0);

    uint8_t back[128];
    t[5] ^= 0x80;
    check("tampered transport rejected", noise_aead_decrypt(V_K1, 0, NULL, 0, t, (size_t)tn, back, sizeof back) < 0);
    check("wrong nonce rejected", noise_aead_decrypt(V_K1, 1, NULL, 0, V_T1, sizeof V_T1, back, sizeof back) < 0);

    // Low-order point (all zero) as remote static: DH output zero must be refused.
    uint8_t zero[32] = {0};
    noise_ik_init(&ini, true, V_PROLOGUE, sizeof V_PROLOGUE, &is, zero);
    check("low-order remote key rejected", noise_ik_write_msg1(&ini, NULL, 0, m1, sizeof m1) < 0);
}

// ---------------------------------------------------------------------------
// Replay window
// ---------------------------------------------------------------------------

static void test_replay(void)
{
    sec_replay_t r = {0};
    check("replay: first", sec_replay_check(&r, 0));
    check("replay: dup", !sec_replay_check(&r, 0));
    check("replay: next", sec_replay_check(&r, 1));
    check("replay: jump", sec_replay_check(&r, 10));
    check("replay: late in window", sec_replay_check(&r, 5));
    check("replay: late dup", !sec_replay_check(&r, 5));
    check("replay: edge 63 back", sec_replay_check(&r, 100) && sec_replay_check(&r, 37));
    check("replay: 64 back rejected", !sec_replay_check(&r, 36));
    check("replay: far jump clears", sec_replay_check(&r, 1000) && sec_replay_check(&r, 999) && !sec_replay_check(&r, 1000));
}

// ---------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------

static void test_pins(void)
{
    sec_pins_t p;
    memset(&p, 0, sizeof p);
    uint8_t a[32], b[32], c[32];
    memset(a, 0xaa, 32);
    memset(b, 0xbb, 32);
    memset(c, 0xcc, 32);
    bool ch;
    check("pin: unknown outside window", sec_pins_check(&p, a, 100, 1, SEC_PIN_CLU, false, &ch) == SEC_PIN_UNKNOWN && !ch);
    check("pin: added in window", sec_pins_check(&p, a, 100, 1, SEC_PIN_CLU, true, &ch) == SEC_PIN_ADDED && ch);
    check("pin: known ok", sec_pins_check(&p, a, 100, 1, SEC_PIN_CLU, false, &ch) == SEC_PIN_OK && !ch);
    check("pin: ip refresh", sec_pins_check(&p, a, 100, 2, SEC_PIN_CLU, false, &ch) == SEC_PIN_OK && ch &&
                                 sec_pins_by_ip(&p, 2) && !sec_pins_by_ip(&p, 1));
    check("pin: sn with other key = conflict", sec_pins_check(&p, b, 100, 3, SEC_PIN_CLU, true, &ch) == SEC_PIN_CONFLICT);
    check("pin: host key (sn 0)", sec_pins_check(&p, b, 0, 9, SEC_PIN_HOST, true, &ch) == SEC_PIN_ADDED);
    check("pin: second host key", sec_pins_check(&p, c, 0, 10, SEC_PIN_HOST, true, &ch) == SEC_PIN_ADDED);
    // c takes over a's address: a's ip cleared
    uint8_t d[32];
    memset(d, 0xdd, 32);
    check("pin: new key takes ip over", sec_pins_check(&p, d, 200, 2, SEC_PIN_CLU, true, &ch) == SEC_PIN_ADDED &&
                                            sec_pins_by_ip(&p, 2) == sec_pins_by_pub(&p, d) && sec_pins_by_sn(&p, 100)->peer_ip == 0);

    char fp[17];
    sec_fingerprint(a, fp);
    check("fingerprint 16 hex", strlen(fp) == 16);
    check("forget by sn", sec_pins_forget(&p, "200") == 1 && !sec_pins_by_pub(&p, d));
    char prefix[9];
    memcpy(prefix, fp, 8);
    prefix[8] = 0;
    check("forget by fingerprint prefix", sec_pins_forget(&p, prefix) == 1 && !sec_pins_by_pub(&p, a));
    check("forget: short prefix ignored", sec_pins_forget(&p, "abc") == 0);

    sec_pins_t full;
    memset(&full, 0, sizeof full);
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_PINS; i++) {
        uint8_t k[32];
        memset(k, i + 1, 32);
        sec_pins_check(&full, k, (uint32_t)(i + 1), 0, SEC_PIN_CLU, true, &ch);
    }
    check("pin: table full", sec_pins_check(&full, c, 999, 0, SEC_PIN_CLU, true, &ch) == SEC_PIN_FULL);
}

// ---------------------------------------------------------------------------
// Two engines over a fake network
// ---------------------------------------------------------------------------

#define IP_A 0x0a000001u
#define IP_B 0x0a000002u
#define IP_X 0x0a000009u
#define PORT 1235

typedef struct {
    uint32_t to, from;
    uint16_t port;
    uint8_t buf[1600];
    size_t len;
} pkt_t;

static pkt_t g_q[32];
static int g_qn;
static pkt_t g_last; // last packet sent, kept for replay tests

typedef struct node {
    sec_engine_t e;
    uint32_t ip;
    bool allow;
    int pins_changed;
    int hs_ok, hs_fail;
    char got[256];
    uint32_t got_handle;
    int delivered;
    bool echo;
    uint64_t wall;
} node_t;

static node_t A, B;
static node_t *g_extra; // optional third node reachable by pump()

static void n_send(void *ctx, uint32_t ip, uint16_t port, const uint8_t *buf, size_t len)
{
    node_t *n = ctx;
    pkt_t *p = &g_q[g_qn++];
    p->to = ip;
    p->from = n->ip;
    p->port = port;
    memcpy(p->buf, buf, len);
    p->len = len;
    g_last = *p;
}

static uint32_t g_now = 1000;

static void n_deliver(void *ctx, uint32_t handle, const sec_session_t *s, const uint8_t *plain, size_t len)
{
    (void)s;
    node_t *n = ctx;
    snprintf(n->got, sizeof n->got, "%.*s", (int)len, (const char *)plain);
    n->got_handle = handle;
    n->delivered++;
    if (n->echo) {
        char r[300];
        int rn = snprintf(r, sizeof r, "echo:%s", n->got);
        sec_engine_send(&n->e, handle, (const uint8_t *)r, (size_t)rn, g_now);
    }
}

static void n_done(void *ctx, uint32_t ip, bool ok)
{
    (void)ip;
    node_t *n = ctx;
    if (ok) {
        n->hs_ok++;
    } else {
        n->hs_fail++;
    }
}

static bool n_allow(void *ctx)
{
    return ((node_t *)ctx)->allow;
}

static void n_pins(void *ctx)
{
    ((node_t *)ctx)->pins_changed++;
}

static void n_random(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    psa_generate_random(out, len);
}

static uint64_t n_wall(void *ctx)
{
    return ((node_t *)ctx)->wall;
}

static void node_init(node_t *n, uint32_t ip, uint32_t sn)
{
    memset(n, 0, sizeof *n);
    n->ip = ip;
    noise_keypair_t kp;
    noise_keypair_generate(&kp);
    sec_ops_t ops = {n_send, n_deliver, n_done, n_allow, n_pins, n_random, n, n_wall};
    sec_engine_init(&n->e, &kp, sn, &ops);
}

/** @brief Deliver queued packets until the network is quiet. */
static void pump(void)
{
    for (int guard = 0; g_qn && guard < 100; guard++) {
        pkt_t p = g_q[0];
        memmove(g_q, g_q + 1, sizeof(pkt_t) * (size_t)--g_qn);
        node_t *dst = p.to == A.ip ? &A : p.to == B.ip ? &B : g_extra && p.to == g_extra->ip ? g_extra : NULL;
        if (dst) {
            sec_engine_input(&dst->e, p.from, PORT, p.buf, p.len, g_now);
        }
    }
}

static void test_engines(void)
{
    node_init(&A, IP_A, 1001);
    node_init(&B, IP_B, 2002);
    B.echo = true;

    // Probe: B answers with its SN and static key.
    uint8_t probe[SEC_PROBE_LEN];
    sec_probe_build(0x1234, probe);
    sec_engine_input(&B.e, IP_A, PORT, probe, sizeof probe, g_now);
    uint32_t sn = 0;
    uint8_t pub[32];
    check("probe answered", g_qn == 1 && sec_probe_parse(g_q[0].buf, g_q[0].len, 0x1234, &sn, pub) && sn == 2002 &&
                                memcmp(pub, B.e.local_static.pub, 32) == 0);
    check("probe nonce mismatch", !sec_probe_parse(g_q[0].buf, g_q[0].len, 0x9999, &sn, pub));
    g_qn = 0;
    sec_engine_input(&B.e, IP_A, PORT, probe, 10, g_now);
    check("short probe ignored (no amplification)", g_qn == 0);

    // Handshake refused: B not in its setup window and A unknown.
    check("connect", sec_engine_connect(&A.e, IP_B, PORT, B.e.local_static.pub, g_now) == 0);
    check("pending", sec_engine_pending(&A.e, IP_B));
    pump();
    check("unknown initiator refused silently", A.hs_ok == 0 && !sec_engine_session_for(&A.e, IP_B, g_now, NULL));
    g_now += SEC_HS_TIMEOUT_MS + 1;
    sec_engine_tick(&A.e, g_now);
    check("pending handshake times out", A.hs_fail == 1 && !sec_engine_pending(&A.e, IP_B));

    // Pairing window open on B: pinned and established.
    B.allow = true;
    sec_engine_connect(&A.e, IP_B, PORT, B.e.local_static.pub, g_now);
    pump();
    check("handshake ok", A.hs_ok == 1 && sec_engine_session_for(&A.e, IP_B, g_now, NULL));
    check("responder pins only after the first DATA", B.pins_changed == 0 && !sec_pins_by_sn(&B.e.pins, 1001));
    B.allow = false; // the window may close in between: the handshake already proved it was open

    // Data both ways.
    bool rekey = true;
    const sec_session_t *s = sec_engine_session_for(&A.e, IP_B, g_now, &rekey);
    check("no rekey on fresh session", s && !rekey);
    uint32_t h = s->local_idx;
    const char *msg = "req:10.0.0.2:00000001:1+1";
    check("send", sec_engine_send(&A.e, h, (const uint8_t *)msg, strlen(msg), g_now) == 0);
    pkt_t sent = g_last;
    pump();
    check("delivered to B", B.delivered == 1 && strcmp(B.got, msg) == 0);
    check("responder pinned initiator on first DATA", B.pins_changed == 1 && sec_pins_by_sn(&B.e.pins, 1001) &&
                                                          sec_pins_by_sn(&B.e.pins, 1001)->peer_ip == IP_A);
    check("echo back to A", A.delivered == 1 && strncmp(A.got, "echo:", 5) == 0);

    // Replay of the captured DATA is dropped.
    sec_engine_input(&B.e, sent.from, PORT, sent.buf, sent.len, g_now);
    check("replayed DATA dropped", B.delivered == 1 && g_qn == 0);
    // Tampered DATA dropped.
    sent.buf[SEC_DATA_HDR + 2] ^= 1;
    sec_engine_input(&B.e, sent.from, PORT, sent.buf, sent.len, g_now);
    check("tampered DATA dropped", B.delivered == 1);
    // Header (counter) tampering breaks the AAD.
    sent.buf[SEC_DATA_HDR + 2] ^= 1;
    sent.buf[5] ^= 0x10;
    sec_engine_input(&B.e, sent.from, PORT, sent.buf, sent.len, g_now);
    check("header tamper dropped", B.delivered == 1);

    // Later pairing attempt from an unknown key while closed: refused even though A knows B.
    node_t saveA = A;
    node_init(&A, IP_A, 1001); // same SN, new key: a cloned / replaced device
    sec_engine_connect(&A.e, IP_B, PORT, B.e.local_static.pub, g_now);
    B.allow = true; // even in the window: the SN is pinned with another key
    pump();
    check("new key for pinned SN refused", A.hs_ok == 0);
    B.allow = false;
    A = saveA;
    A.e.ops.ctx = &A;

    // Re-handshake after REKEY_AFTER: initiator asked to rekey, old session still usable.
    g_now += SEC_REKEY_AFTER_MS + 1;
    // keep sessions from idling out
    for (int i = 0; i < CONFIG_CLU_SEC_MAX_SESSIONS; i++) {
        A.e.sess[i].last_ms = g_now;
        B.e.sess[i].last_ms = g_now;
    }
    s = sec_engine_session_for(&A.e, IP_B, g_now, &rekey);
    check("rekey requested after an hour", s && rekey);
    sec_engine_connect(&A.e, IP_B, PORT, B.e.local_static.pub, g_now);
    pump();
    const sec_session_t *s2 = sec_engine_session_for(&A.e, IP_B, g_now, &rekey);
    check("newest session preferred after rekey", s2 && s2->local_idx != h && !rekey);

    // Idle expiry.
    g_now += SEC_IDLE_MS + 1;
    sec_engine_tick(&A.e, g_now);
    sec_engine_tick(&B.e, g_now);
    check("idle sessions expire", sec_engine_session_count(&A.e) == 0 && sec_engine_session_count(&B.e) == 0);

    // Known pins reconnect without a window.
    sec_engine_connect(&A.e, IP_B, PORT, B.e.local_static.pub, g_now);
    pump();
    check("pinned peer reconnects outside window", sec_engine_session_for(&A.e, IP_B, g_now, NULL) != NULL);

    // Lost HS_RESP: the responder keeps no pin; a retransmitted HS_INIT completes the first attempt.
    node_t C;
    node_init(&C, IP_X, 3003);
    B.allow = true;
    g_now += 5000;
    sec_engine_connect(&C.e, IP_B, PORT, B.e.local_static.pub, g_now);
    pump();
    g_qn = 0; // the HS_RESP from B never reaches C... (pump already delivered nothing to IP_X)
    check("no pin without DATA (HS_RESP lost)", !sec_pins_by_sn(&B.e.pins, 3003) && C.hs_ok == 0);
    check("retransmit pending HS_INIT", sec_engine_retransmit(&C.e, IP_B) == 0 && g_qn == 1);
    pkt_t rt = g_q[0];
    g_qn = 0;
    sec_engine_input(&B.e, IP_X, PORT, rt.buf, rt.len, g_now);
    check("responder answers the copy", g_qn == 1 && g_q[0].buf[0] == SEC_T_HS_RESP);
    sec_engine_input(&C.e, IP_B, PORT, g_q[0].buf, g_q[0].len, g_now);
    g_qn = 0;
    check("initiator completes on the copy's HS_RESP", C.hs_ok == 1 && sec_engine_session_for(&C.e, IP_B, g_now, NULL));
    check("responder: unconfirmed sessions not used for sending", sec_engine_session_for(&B.e, IP_X, g_now, NULL) == NULL);
    const sec_session_t *cs = sec_engine_session_for(&C.e, IP_B, g_now, NULL);
    sec_engine_send(&C.e, cs->local_idx, (const uint8_t *)"req:x:00000009:1", 16, g_now);
    pkt_t cd = g_q[0];
    g_qn = 0;
    sec_engine_input(&B.e, IP_X, PORT, cd.buf, cd.len, g_now);
    g_qn = 0;
    const sec_session_t *bs = sec_engine_session_for(&B.e, IP_X, g_now, NULL);
    check("responder uses the session the initiator confirmed", bs && bs->remote_idx == cs->local_idx &&
                                                                    sec_pins_by_sn(&B.e.pins, 3003));
    check("retransmit with nothing pending", sec_engine_retransmit(&C.e, IP_B) == -1);
    B.allow = false;

    // HS_INIT replay protection (timestamps).
    node_t D;
    node_init(&D, IP_X, 4004);
    g_extra = &D;
    D.wall = B.wall = 1700000000000ull;
    B.allow = true;
    g_now += 5000;
    g_qn = 0;
    sec_engine_connect(&D.e, IP_B, PORT, B.e.local_static.pub, g_now);
    pkt_t m1 = g_q[0];
    pump();
    check("timestamped handshake ok", D.hs_ok == 1);
    g_now += 5000;
    g_qn = 0;
    sec_engine_input(&B.e, IP_X, PORT, m1.buf, m1.len, g_now);
    check("replayed HS_INIT refused", g_qn == 0);
    sec_engine_connect(&D.e, IP_B, PORT, B.e.local_static.pub, g_now); // same wall clock: ts still increases
    pump();
    check("next handshake accepted (ts strictly increasing)", D.hs_ok == 2);
    D.wall = 1700000000000ull + 4 * 3600 * 1000;
    g_now += 5000;
    g_qn = 0;
    sec_engine_connect(&D.e, IP_B, PORT, B.e.local_static.pub, g_now);
    pump();
    check("HS_INIT far from our clock refused", D.hs_ok == 2);
    D.wall = B.wall = 0;
    g_extra = NULL;
    B.allow = false;
    g_qn = 0;

    // Responder handshake budget.
    node_t X;
    node_init(&X, IP_X, 0);
    int answered = 0;
    B.allow = true;
    g_now += 5000; // bucket full again
    for (int i = 0; i < 20; i++) {
        g_qn = 0;
        sec_engine_connect(&X.e, IP_B, PORT, B.e.local_static.pub, g_now);
        pkt_t p = g_q[0];
        g_qn = 0;
        sec_engine_input(&B.e, IP_X, PORT, p.buf, p.len, g_now);
        answered += g_qn;
    }
    check("handshake flood limited to the per-second budget", answered == (int)SEC_HS_RATE_PER_S);
    B.allow = false;
    g_qn = 0;
}

int main(void)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        printf("FAIL psa_crypto_init\n");
        return 1;
    }
    test_noise_vectors();
    test_replay();
    test_pins();
    test_engines();
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
