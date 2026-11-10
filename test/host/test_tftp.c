// Host tests for the TFTP server (clu_tftp.c, included: its handlers are static). Requests arrive on
// the listening socket from a script; each transfer talks to a fake peer that reacts to what the
// server sends (ACKs DATA for downloads, sends the next DATA per ACK for uploads), with knobs for
// loss, duplicates, stray hosts and errors. Time is simulated: an empty socket waits out its
// SO_RCVTIMEO. Files live in a temporary directory (the TFTP root).
#define _GNU_SOURCE // nftw
#include <errno.h>
#include <ftw.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../components/clu_tftp/clu_tftp.c"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- clock, task escape
static int64_t s_now;
int64_t esp_timer_get_time(void) { return s_now; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) { return ESP_FAIL; }
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us) { return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t h) { return ESP_OK; }
esp_err_t esp_timer_delete(esp_timer_handle_t h) { return ESP_OK; }

static jmp_buf s_escape;
static void block_forever(void) { longjmp(s_escape, 1); }
static void run_task(void)
{
    if (setjmp(s_escape) == 0) {
        tftp_task(NULL);
    }
}
void vTaskDelete(TaskHandle_t t) { block_forever(); }
static void (*s_task_fn)(void *);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *n, uint32_t s, void *a, UBaseType_t p,
                                   TaskHandle_t *h, BaseType_t c)
{
    s_task_fn = fn;
    return pdTRUE;
}

// ---------------------------------------------------------------- fake sockets
#define FD_LISTEN 3
#define PEER_IP   "192.168.1.50"
#define PEER_PORT 50000

typedef struct {
    uint8_t data[600];
    size_t len;
    struct sockaddr_in from;
} pkt_t;
typedef struct {
    pkt_t q[64];
    int head, n;
    int64_t timeout_us;
} sock_t;
static sock_t s_socks[16];
static int s_next_fd = FD_LISTEN;
static int s_sock_fail_at = -1; // socket() fails when the next fd would be this
static int s_bind_fail;

static struct sockaddr_in addr(const char *ip, int port)
{
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_aton(ip, &a.sin_addr);
    return a;
}

static void push(int fd, const void *data, size_t len, struct sockaddr_in from)
{
    sock_t *s = &s_socks[fd];
    pkt_t *p = &s->q[(s->head + s->n++) % 64];
    memcpy(p->data, data, len);
    p->len = len;
    p->from = from;
}

int host_socket(int d, int t, int p)
{
    if (s_next_fd == s_sock_fail_at) {
        s_sock_fail_at = -1;
        return -1;
    }
    if (s_next_fd != FD_LISTEN) { // the listening socket keeps the scripted requests
        memset(&s_socks[s_next_fd], 0, sizeof s_socks[0]);
    }
    return s_next_fd++;
}
int host_bind(int s, const struct sockaddr *a, socklen_t l) { return s_bind_fail ? -1 : 0; }
int host_setsockopt(int s, int level, int opt, const void *v, socklen_t l)
{
    const struct timeval *tv = v;
    s_socks[s].timeout_us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
    return 0;
}
static int s_closed;
int host_close(int s) { s_closed++; return 0; }

ssize_t host_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fl)
{
    sock_t *s = &s_socks[fd];
    if (!s->n) {
        if (fd == FD_LISTEN) {
            block_forever(); // the server waits for the next request
        }
        s_now += s->timeout_us ? s->timeout_us : 5000000; // SO_RCVTIMEO expires
        errno = EAGAIN;
        return -1;
    }
    pkt_t *p = &s->q[s->head];
    s->head = (s->head + 1) % 64;
    s->n--;
    size_t n = p->len < len ? p->len : len;
    memcpy(buf, p->data, n);
    memcpy(from, &p->from, sizeof p->from);
    *fl = sizeof p->from;
    return (ssize_t)n;
}

// Errors sent on the listening socket (request rejected) and on transfer sockets.
static uint16_t s_listen_err, s_xfer_err;
static int s_listen_errs, s_stray_errs;

// ---------------------------------------------------------------- fake peer
static struct {
    bool upload;            // WRQ: we send DATA, else RRQ: we ACK DATA
    // download
    uint8_t got[4096];
    size_t got_len;
    int blocks, last_len;
    int drop_ack_block;     // do not ACK this block the first time
    int dup_ack_block;      // ACK the previous block before ACKing this one
    int stray_at_block;     // a packet from another port before the ACK of this block
    int error_at_block;     // answer this block with ERROR
    bool silent;            // never answer
    // upload
    const uint8_t *src;
    size_t src_len;
    int sent_block;
    int resend_block;       // send this block twice
    int skip_block;         // do not send this block the first time (server times out)
    int ack_at_block;       // send an ACK instead of this DATA block
    int short_at_block;     // send a 3-byte packet before this block
    int perror_at_block;    // send ERROR instead of this block
} P;

static void peer_reset(void)
{
    memset(&P, 0, sizeof P);
}

static void peer_send(int fd, const uint8_t *pkt, size_t len)
{
    push(fd, pkt, len, addr(PEER_IP, PEER_PORT));
}

static void peer_data(int fd, int block)
{
    uint8_t pkt[4 + 512];
    size_t off = (size_t)(block - 1) * 512;
    size_t n = off >= P.src_len ? 0 : (P.src_len - off < 512 ? P.src_len - off : 512);
    tftp_data_hdr(pkt, (uint16_t)block);
    memcpy(pkt + 4, P.src + off, n);
    peer_send(fd, pkt, 4 + n);
}

static void on_server_send(int fd, const uint8_t *pkt, size_t len, const struct sockaddr_in *to)
{
    uint16_t op = (uint16_t)(pkt[0] << 8 | pkt[1]);
    uint16_t b = (uint16_t)(pkt[2] << 8 | pkt[3]);
    if (op == TFTP_ERROR) {
        if (fd == FD_LISTEN) {
            s_listen_err = b;
            s_listen_errs++;
        } else if (to->sin_port != htons(PEER_PORT)) {
            s_stray_errs++;
        } else {
            s_xfer_err = b;
        }
        return;
    }
    if (P.silent || fd == FD_LISTEN) {
        return;
    }
    if (!P.upload && op == TFTP_DATA) {
        if (b == P.drop_ack_block) {
            P.drop_ack_block = 0;
            return;
        }
        if (b == P.error_at_block) {
            uint8_t e[32];
            peer_send(fd, e, tftp_error(e, sizeof e, TFTP_E_UNDEFINED, "cancel"));
            return;
        }
        if (b == P.stray_at_block) {
            P.stray_at_block = 0;
            uint8_t a[4];
            for (int i = 0; i < 6; i++) { // 6 strays: only 4 get an error
                push(fd, a, tftp_ack(a, b), addr(PEER_IP, PEER_PORT + 1));
            }
        }
        if (b == P.dup_ack_block) {
            uint8_t a[4];
            peer_send(fd, a, tftp_ack(a, (uint16_t)(b - 1)));
        }
        if (b == P.blocks + 1) { // new block (not a retransmission)
            memcpy(P.got + P.got_len, pkt + 4, len - 4);
            P.got_len += len - 4;
            P.blocks++;
            P.last_len = (int)(len - 4);
        }
        uint8_t a[4];
        peer_send(fd, a, tftp_ack(a, b));
        return;
    }
    if (P.upload && op == TFTP_ACK) {
        int next = b + 1;
        if (b < P.sent_block || (size_t)P.sent_block * 512 > P.src_len) {
            return; // old ACK, or the last block was already acknowledged
        }
        if (next == P.skip_block) {
            P.skip_block = 0;
            return;
        }
        if (next == P.perror_at_block) {
            uint8_t e[32];
            peer_send(fd, e, tftp_error(e, sizeof e, TFTP_E_UNDEFINED, "cancel"));
            return;
        }
        if (next == P.ack_at_block) {
            uint8_t a[4];
            peer_send(fd, a, tftp_ack(a, 1));
            return;
        }
        if (next == P.short_at_block) {
            P.short_at_block = 0;
            peer_send(fd, (const uint8_t *)"\0\3\0", 3);
        }
        peer_data(fd, next);
        if (next == P.resend_block) {
            peer_data(fd, next);
        }
        P.sent_block = next;
    }
}

ssize_t host_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tl)
{
    on_server_send(fd, buf, len, (const struct sockaddr_in *)to);
    return (ssize_t)len;
}

// ---------------------------------------------------------------- helpers
static char s_tmp[32];

static void request(uint16_t op, const char *fn, const char *mode, const char *from_ip)
{
    uint8_t b[128];
    b[0] = 0;
    b[1] = (uint8_t)op;
    size_t n = 2;
    n += (size_t)sprintf((char *)b + n, "%s", fn) + 1;
    n += (size_t)sprintf((char *)b + n, "%s", mode) + 1;
    push(FD_LISTEN, b, n, addr(from_ip, PEER_PORT));
}

// Run one request through the server task.
static void serve(uint16_t op, const char *fn, const char *mode)
{
    s_next_fd = FD_LISTEN;
    s_xfer_err = 0xffff;
    request(op, fn, mode, PEER_IP);
    run_task();
}

static void write_file(const char *rel, const void *data, size_t len)
{
    char p[128];
    snprintf(p, sizeof p, "%s/%s", s_tmp, rel);
    FILE *f = fopen(p, "wb");
    fwrite(data, 1, len, f);
    fclose(f);
}

static size_t read_file(const char *rel, uint8_t *out, size_t cap)
{
    char p[128];
    snprintf(p, sizeof p, "%s/%s", s_tmp, rel);
    FILE *f = fopen(p, "rb");
    if (!f) {
        return (size_t)-1;
    }
    size_t n = fread(out, 1, cap, f);
    fclose(f);
    return n;
}

static bool exists(const char *rel)
{
    char p[128];
    snprintf(p, sizeof p, "%s/%s", s_tmp, rel);
    return access(p, F_OK) == 0;
}

static char s_uploaded[128];
static int s_uploads;
static void on_upload(const char *path)
{
    snprintf(s_uploaded, sizeof s_uploaded, "%s", path);
    s_uploads++;
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) { return remove(path); }

// ---------------------------------------------------------------- tests
static void test_requests(void)
{
    s_bind_fail = 1;
    run_task();
    check("task: bind failure -> task deleted", s_closed == 0);
    s_bind_fail = 0;

    s_next_fd = FD_LISTEN;
    request(TFTP_RRQ, "a:\\CONFIG.TXT", "octet", PEER_IP);
    run_task();
    check("disabled: request refused (access violation)", s_listen_errs == 1 && s_listen_err == TFTP_E_ACCESS);

    s_next_fd = FD_LISTEN;
    request(TFTP_ACK, "x", "octet", PEER_IP);
    push(FD_LISTEN, "\0\1", 2, addr(PEER_IP, PEER_PORT)); // truncated RRQ
    run_task();
    check("illegal operation / malformed request -> error 4", s_listen_errs == 3 && s_listen_err == TFTP_E_ILLEGAL);

    clu_tftp_enable(true, inet_addr(PEER_IP));
    check("enabled for one host", clu_tftp_enabled());
    s_next_fd = FD_LISTEN;
    request(TFTP_RRQ, "a:\\CONFIG.TXT", "octet", "192.168.1.66");
    request(TFTP_RRQ, "a:\\..\\x", "octet", PEER_IP);
    request(TFTP_WRQ, "a:\\sub\\x", "octet", PEER_IP);
    run_task();
    check("other host / path escape / subdirectory -> access violation",
          s_listen_errs == 6 && s_listen_err == TFTP_E_ACCESS);

    s_now += (int64_t)CLU_TFTP_IDLE_S * 1000000 + 1;
    check("idle timeout disables TFTP", !clu_tftp_enabled());
    clu_tftp_enable(false, 0);
    clu_tftp_enable(true, inet_addr(PEER_IP));
}

static void test_download(void)
{
    uint8_t data[1200], got[4096];
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(i * 7);
    }
    write_file("a/BIG.BIN", data, sizeof data);
    peer_reset();
    serve(TFTP_RRQ, "a:\\BIG.BIN", "octet");
    check("RRQ octet: 3 blocks (512, 512, 176), content intact",
          P.blocks == 3 && P.last_len == 176 && P.got_len == sizeof data && !memcmp(P.got, data, sizeof data));

    write_file("a/E512.BIN", data, 512);
    peer_reset();
    serve(TFTP_RRQ, "/a/E512.BIN", "OCTET");
    check("RRQ of exactly 512 bytes: final empty block", P.blocks == 2 && P.last_len == 0 && P.got_len == 512);

    write_file("a/EMPTY.TXT", "", 0);
    peer_reset();
    serve(TFTP_RRQ, "a:\\EMPTY.TXT", "octet");
    check("RRQ of an empty file: one empty block", P.blocks == 1 && P.last_len == 0);

    write_file("a/T.TXT", "a\nb\rc", 5);
    peer_reset();
    serve(TFTP_RRQ, "a:\\T.TXT", "netascii");
    check("RRQ netascii: LF -> CR LF, CR -> CR NUL", P.got_len == 7 && !memcmp(P.got, "a\r\nb\r\0c", 7));

    peer_reset();
    serve(TFTP_RRQ, "a:\\NOPE.TXT", "octet");
    check("RRQ of a missing file -> error 1 on the transfer socket", s_xfer_err == TFTP_E_NOT_FOUND && !P.blocks);

    // lost ACK, duplicate ACK, stray host
    peer_reset();
    P.drop_ack_block = 2;
    P.dup_ack_block = 3;
    P.stray_at_block = 1;
    s_stray_errs = 0;
    serve(TFTP_RRQ, "a:\\BIG.BIN", "octet");
    check("RRQ: lost ACK -> block resent; duplicate ACK ignored", P.blocks == 3 && P.got_len == sizeof data &&
                                                                        !memcmp(P.got, data, sizeof data));
    check("RRQ: stray host gets 'unknown TID' (first 4 packets only)", s_stray_errs == 4);

    peer_reset();
    P.error_at_block = 2;
    serve(TFTP_RRQ, "a:\\BIG.BIN", "octet");
    check("RRQ: peer ERROR aborts the transfer", P.blocks == 1);

    peer_reset();
    P.silent = true;
    int64_t t0 = s_now;
    serve(TFTP_RRQ, "a:\\BIG.BIN", "octet");
    check("RRQ: silent peer -> 4 tries x 5 s, then give up", s_now - t0 >= 4 * 5000000LL && s_now - t0 < 5 * 5000000LL);

    // memory: PSRAM allocation fails -> internal heap; netascii buffer fails -> error
    peer_reset();
    host_heap_fail = 1;
    serve(TFTP_RRQ, "a:\\BIG.BIN", "octet");
    check("RRQ: no PSRAM -> internal heap used", P.got_len == sizeof data);
    peer_reset();
    host_heap_fail = 2; // file buffer: PSRAM fails, internal heap used; netascii buffer fails
    serve(TFTP_RRQ, "a:\\T.TXT", "netascii");
    host_heap_fail = 0;
    check("RRQ netascii: out of memory -> error 0", s_xfer_err == TFTP_E_UNDEFINED && !P.blocks);

    peer_reset();
    s_sock_fail_at = FD_LISTEN + 1; // transfer socket
    serve(TFTP_RRQ, "a:\\BIG.BIN", "octet");
    check("RRQ: no transfer socket -> request dropped", !P.blocks);
    (void)got;
}

static void test_upload(void)
{
    static uint8_t src[1100], got[4096];
    for (size_t i = 0; i < sizeof src; i++) {
        src[i] = (uint8_t)(i * 13 + 1);
    }
    clu_tftp_set_upload_cb(on_upload);
    write_file("a/OM.LUA", "old", 3);

    peer_reset();
    P.upload = true;
    P.src = src;
    P.src_len = sizeof src;
    serve(TFTP_WRQ, "a:\\OM.LUA", "octet");
    char want[128];
    snprintf(want, sizeof want, "%s/a/OM.LUA", s_tmp);
    check("WRQ octet: file replaced, upload callback with its path",
          read_file("a/OM.LUA", got, sizeof got) == sizeof src && !memcmp(got, src, sizeof src) &&
              s_uploads == 1 && !strcmp(s_uploaded, want) && !exists("a/TFTP.TMP"));

    // netascii: CR LF -> LF, CR NUL -> CR, final lone CR kept
    peer_reset();
    P.upload = true;
    P.src = (const uint8_t *)"x\r\ny\r\0z\r";
    P.src_len = 8;
    serve(TFTP_WRQ, "a:\\USER.LUA", "netascii");
    size_t n = read_file("a/USER.LUA", got, sizeof got);
    check("WRQ netascii: CR LF -> LF, CR NUL -> CR, trailing CR kept", n == 6 && !memcmp(got, "x\ny\rz\r", 6));

    // duplicate DATA, lost DATA (server re-ACKs), short packet
    peer_reset();
    P.upload = true;
    P.src = src;
    P.src_len = sizeof src;
    P.resend_block = 1;
    P.skip_block = 2;
    P.short_at_block = 3;
    serve(TFTP_WRQ, "a:\\R.BIN", "octet");
    check("WRQ: duplicate block re-ACKed, lost block re-requested, runt packet ignored",
          read_file("a/R.BIN", got, sizeof got) == sizeof src && !memcmp(got, src, sizeof src));

    int uploads = s_uploads;
    peer_reset();
    P.upload = true;
    P.src = src;
    P.src_len = sizeof src;
    P.perror_at_block = 2;
    serve(TFTP_WRQ, "a:\\OM.LUA", "octet");
    check("WRQ: peer ERROR -> old file kept, temp file removed, no callback",
          read_file("a/OM.LUA", got, sizeof got) == sizeof src && !exists("a/TFTP.TMP") && s_uploads == uploads);

    peer_reset();
    P.upload = true;
    P.src = src;
    P.src_len = sizeof src;
    P.ack_at_block = 2;
    serve(TFTP_WRQ, "a:\\X.BIN", "octet");
    check("WRQ: ACK instead of DATA -> error 4, nothing stored", s_xfer_err == TFTP_E_ILLEGAL && !exists("a/X.BIN"));

    peer_reset();
    P.upload = true;
    P.silent = true;
    int64_t t0 = s_now;
    serve(TFTP_WRQ, "a:\\Y.BIN", "octet");
    check("WRQ: silent peer -> 4 waits of 5 s, nothing stored", !exists("a/Y.BIN") && s_now - t0 >= 4 * 5000000LL);

    peer_reset();
    P.upload = true;
    P.src = src;
    P.src_len = sizeof src;
    serve(TFTP_WRQ, "z:\\NODRIVE.BIN", "octet");
    check("WRQ to a missing drive -> error 2 (cannot create)", s_xfer_err == TFTP_E_ACCESS);

    // larger than CLU_TFTP_MAX_UPLOAD
    size_t big_len = CLU_TFTP_MAX_UPLOAD + 512;
    uint8_t *big = calloc(1, big_len);
    peer_reset();
    P.upload = true;
    P.src = big;
    P.src_len = big_len;
    serve(TFTP_WRQ, "a:\\HUGE.BIN", "octet");
    free(big);
    check("WRQ over the size limit -> error 3, nothing stored", s_xfer_err == TFTP_E_DISK_FULL && !exists("a/HUGE.BIN"));

    peer_reset();
    s_sock_fail_at = FD_LISTEN + 1;
    serve(TFTP_WRQ, "a:\\Z.BIN", "octet");
    check("WRQ: no transfer socket -> request dropped", !exists("a/Z.BIN"));
}

int main(void)
{
    snprintf(s_tmp, sizeof s_tmp, "/tmp/tftpXXXXXX"); // root must fit clu_tftp's 16-byte buffer
    if (!mkdtemp(s_tmp)) {
        perror("mkdtemp");
        return 1;
    }
    char a[64];
    snprintf(a, sizeof a, "%s/a", s_tmp);
    mkdir(a, 0775);

    clu_tftp_init(s_tmp);
    check("init: root stored, task created", !strcmp(s_root, s_tmp) && s_task_fn == tftp_task);
    test_requests();
    test_download();
    test_upload();

    if (strncmp(s_tmp, "/tmp/tftp", 9) == 0) {
        nftw(s_tmp, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
    }
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
