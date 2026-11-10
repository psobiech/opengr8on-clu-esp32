// Host tests for the telnet log console (clu_telnet.c, included: ring buffer, IAC parser and
// server loop are static). Logging goes through the hook installed with esp_log_set_vprintf();
// the server loop runs against fake TCP sockets until it would block in select() with nothing to
// do, then returns to the test (longjmp).
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../components/clu_telnet/clu_telnet.c"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- log hook, task escape
static vprintf_like_t s_hook;
static int s_orig_calls;
static int orig_vprintf(const char *fmt, va_list ap)
{
    s_orig_calls++;
    return 0;
}
vprintf_like_t esp_log_set_vprintf(vprintf_like_t fn)
{
    s_hook = fn;
    return orig_vprintf;
}
static void tlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    s_hook(fmt, ap);
    va_end(ap);
}

static jmp_buf s_escape;
static void block_forever(void) { longjmp(s_escape, 1); }
static int s_idle;
static void run_server(void)
{
    s_idle = 0;
    if (setjmp(s_escape) == 0) {
        server_task(NULL);
    }
}
void vTaskDelete(TaskHandle_t t) { block_forever(); }
static int s_tasks;
BaseType_t xTaskCreate(void (*fn)(void *), const char *n, uint32_t s, void *a, UBaseType_t p, TaskHandle_t *h)
{
    s_tasks++;
    return pdTRUE;
}

// ---------------------------------------------------------------- fake TCP
#define LS 3
static int s_sock_fail, s_bind_fail, s_listen_fail;
static int s_next_fd = 10;

typedef struct {
    char ip[16];
    bool open;
    char out[8192];      // bytes the server sent
    size_t out_len;
    uint8_t in[256];     // bytes waiting to be received
    size_t in_len;
    int recv_result;     // with in_len 0 and pending: 0 = peer closed, -1 = error (recv_errno)
    int recv_errno;
    bool pending;        // readable (data, EOF or error)
    int send_fail;       // send() fails
    int send_max;        // partial sends of at most this many bytes (0 = all)
} conn_t;
static conn_t s_conn[32];
static int s_accept_q[8], s_naccept;

int host_socket(int d, int t, int p) { return s_sock_fail ? -1 : LS; }
int host_bind(int s, const struct sockaddr *a, socklen_t l) { return s_bind_fail ? -1 : 0; }
int host_listen(int s, int b) { return s_listen_fail ? -1 : 0; }
int host_setsockopt(int s, int level, int opt, const void *v, socklen_t l) { return 0; }
int host_close(int s)
{
    if (s != LS) {
        s_conn[s].open = false;
    }
    return 0;
}
ssize_t host_sendto(int s, const void *b, size_t l, int f, const struct sockaddr *to, socklen_t tl) { return -1; }
ssize_t host_recvfrom(int s, void *b, size_t l, int f, struct sockaddr *fr, socklen_t *fl) { return -1; }

// A peer connects from ip; returns its fd.
static int connect_from(const char *ip)
{
    int fd = s_next_fd++;
    memset(&s_conn[fd], 0, sizeof s_conn[fd]);
    snprintf(s_conn[fd].ip, sizeof s_conn[fd].ip, "%s", ip);
    s_conn[fd].open = true;
    s_accept_q[s_naccept++] = fd;
    return fd;
}

int host_accept(int s, struct sockaddr *a, socklen_t *al)
{
    if (!s_naccept) {
        errno = EAGAIN;
        return -1;
    }
    int fd = s_accept_q[0];
    memmove(s_accept_q, s_accept_q + 1, sizeof s_accept_q[0] * (size_t)--s_naccept);
    struct sockaddr_in *in = (struct sockaddr_in *)a;
    memset(in, 0, sizeof *in);
    in->sin_family = AF_INET;
    inet_aton(s_conn[fd].ip, &in->sin_addr);
    *al = sizeof *in;
    return fd;
}

ssize_t host_send(int fd, const void *buf, size_t len, int flags)
{
    conn_t *c = &s_conn[fd];
    if (c->send_fail) {
        return -1;
    }
    size_t n = c->send_max && len > (size_t)c->send_max ? (size_t)c->send_max : len;
    if (c->out_len + n < sizeof c->out) {
        memcpy(c->out + c->out_len, buf, n);
        c->out_len += n;
        c->out[c->out_len] = 0;
    }
    return (ssize_t)n;
}

ssize_t host_recv(int fd, void *buf, size_t len, int flags)
{
    conn_t *c = &s_conn[fd];
    c->pending = false;
    if (c->in_len) {
        size_t n = c->in_len < len ? c->in_len : len;
        memcpy(buf, c->in, n);
        memmove(c->in, c->in + n, c->in_len - n);
        c->in_len -= n;
        c->pending = c->in_len > 0;
        return (ssize_t)n;
    }
    errno = c->recv_errno;
    return c->recv_result;
}

static int s_selects;
int host_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    s_selects++;
    int ready = 0;
    bool ls_set = FD_ISSET(LS, r);
    FD_ZERO(r);
    if (ls_set && s_naccept) {
        FD_SET(LS, r);
        ready++;
    }
    for (int fd = 10; fd < n; fd++) {
        if (s_conn[fd].open && s_conn[fd].pending) {
            FD_SET(fd, r);
            ready++;
        }
    }
    if (!ready) {
        // 50 ms timeout: the server flushes clients after it; a second idle round means
        // everything is sent and nothing is pending -> the task would just keep polling.
        if (s_idle++) {
            block_forever();
        }
        return 0;
    }
    s_idle = 0;
    return ready;
}

static void input(int fd, const void *data, size_t len)
{
    memcpy(s_conn[fd].in + s_conn[fd].in_len, data, len);
    s_conn[fd].in_len += len;
    s_conn[fd].pending = true;
}

static void peer_eof(int fd, int result, int err)
{
    s_conn[fd].recv_result = result;
    s_conn[fd].recv_errno = err;
    s_conn[fd].pending = true;
}

static void reset_out(int fd)
{
    s_conn[fd].out_len = 0;
    s_conn[fd].out[0] = 0;
}

// ---------------------------------------------------------------- tests
int main(void)
{
    clu_telnet_cfg_t cfg = {.port = 23, .max_clients = 2, .history = 300, .allow = "192.168.1.0/24"};

    host_heap_fail = 1;
    clu_telnet_init(&cfg);
    clu_telnet_start();
    check("init: no memory for the history -> console off, no task", !s_buf && !s_hook && s_tasks == 0);

    clu_telnet_init(&cfg);
    clu_telnet_start();
    check("init: history buffer, log hook installed, task started", s_buf && s_size == 300 && s_hook && s_tasks == 1);

    tlog("boot %d\n", 1);
    tlog("crlf already\r\n");
    check("log hook: forwards to the original output", s_orig_calls == 2);
    check("ring: LF -> CR LF, existing CR LF kept", s_head == 22 && !memcmp(s_buf, "boot 1\r\ncrlf already\r\n", 22));

    // ---- server start failures
    s_sock_fail = 1;
    run_server();
    s_sock_fail = 0;
    s_bind_fail = 1;
    run_server();
    s_bind_fail = 0;
    s_listen_fail = 1;
    run_server();
    s_listen_fail = 0;
    check("server: socket / bind / listen failure -> task deleted", s_selects == 0);

    // ---- allowed client: history replay
    int c1 = connect_from("192.168.1.50");
    int bad = connect_from("10.0.0.1");
    run_server();
    check("client in the allowed subnet: history replayed",
          s_conn[c1].open && !strcmp(s_conn[c1].out, "boot 1\r\ncrlf already\r\n"));
    check("client outside CLU_TELNET_ALLOW: closed, nothing sent", !s_conn[bad].open && s_conn[bad].out_len == 0);

    // ---- live lines, long line cut
    reset_out(c1);
    char longline[400];
    memset(longline, 'x', sizeof longline - 1);
    longline[sizeof longline - 1] = 0;
    tlog("%s\n", longline);
    run_server();
    check("long line cut to 255 bytes ending in '...'",
          s_conn[c1].out_len == 256 && !memcmp(s_conn[c1].out + 251, "...\r\n", 5));

    // ---- too many clients
    int c2 = connect_from("192.168.1.51");
    int c3 = connect_from("192.168.1.52");
    run_server();
    check("third client: 'too many log clients', closed",
          s_conn[c2].open && !s_conn[c3].open && strstr(s_conn[c3].out, "too many log clients"));

    // ---- telnet input: IAC negotiation / subnegotiation / NOP ignored, Ctrl-C / Ctrl-D / IP close
    static const uint8_t iac[] = {255, 251, 1, 255, 250, 24, 1, 255, 255, 1, 255, 240, 255, 241, 'h', 'i',
                                  0, 1, 2, 5, 6, 7, 8, 9}; // > 16 bytes: logged truncated
    input(c1, iac, sizeof iac);
    run_server();
    check("IAC WILL / SB..SE / NOP and plain text ignored: client stays", s_conn[c1].open && s_cli[0].iac == IAC_NONE);
    input(c1, (const uint8_t[]){0x03}, 1);
    input(c2, (const uint8_t[]){'a', 0x04}, 2);
    run_server();
    check("Ctrl-C / Ctrl-D close the client", !s_conn[c1].open && !s_conn[c2].open);

    int c4 = connect_from("192.168.1.53");
    int c5 = connect_from("192.168.1.54");
    run_server();
    input(c4, (const uint8_t[]){255, 244}, 2); // IAC IP
    peer_eof(c5, -1, EAGAIN);                  // spurious wakeup
    run_server();
    check("IAC IP closes; EAGAIN keeps the client", !s_conn[c4].open && s_conn[c5].open);
    peer_eof(c5, 0, 0);
    run_server();
    check("peer close (recv 0) closes the client", !s_conn[c5].open);
    int c6 = connect_from("192.168.1.55");
    run_server();
    peer_eof(c6, -1, ECONNRESET);
    run_server();
    check("recv error closes the client", !s_conn[c6].open);

    // ---- ring wrap: lost bytes reported, new client replays from a line boundary
    int c7 = connect_from("192.168.1.56");
    run_server();
    reset_out(c7);
    for (int i = 0; i < 40; i++) {
        tlog("line %02d\n", i); // 40 x 9 bytes > 300
    }
    run_server();
    check("slow client: '[... N bytes dropped]' then the newest lines",
          strstr(s_conn[c7].out, "bytes dropped]\r\n") && strstr(s_conn[c7].out, "line 39\r\n"));
    int c8 = connect_from("192.168.1.57");
    run_server();
    check("new client after wrap: replay starts at a line start", !strncmp(s_conn[c8].out, "line ", 5) &&
                                                                    strstr(s_conn[c8].out, "line 39\r\n"));

    // ---- partial sends / send failure
    s_conn[c8].send_max = 3;
    reset_out(c8);
    tlog("partial\n");
    run_server();
    check("partial sends completed", !strcmp(s_conn[c8].out, "partial\r\n"));
    s_conn[c7].send_fail = 1;
    tlog("x\n");
    run_server();
    check("send failure closes the client", !s_conn[c7].open);
    s_conn[c8].send_fail = 1;
    for (int i = 0; i < 40; i++) {
        tlog("again %02d\n", i);
    }
    run_server();
    check("send failure on the 'dropped' note closes the client", !s_conn[c8].open);

    // ---- CLU_TELNET_ALLOW forms
    static const struct { const char *allow, *ip; bool ok; } allow[] = {
        {"", "8.8.8.8", true},          {"192.168.1.9", "192.168.1.9", true}, {"192.168.1.9", "192.168.1.10", false},
        {"0.0.0.0/0", "1.2.3.4", true}, {"10.0.0.0/8", "10.9.9.9", true},    {"1.2.3.4/40", "1.2.3.4", false},
        {"300.1.1.1", "8.8.8.8", false}, {"1.2.3.4/-1", "1.2.3.4", false},   {"123456789012345678", "1.1.1.1", false},
    };
    bool all = true;
    for (size_t i = 0; i < sizeof allow / sizeof allow[0]; i++) {
        parse_allow(allow[i].allow);
        uint32_t ip = inet_addr(allow[i].ip);
        bool ok = (ip & s_allow_mask) == (s_allow_ip & s_allow_mask);
        if (ok != allow[i].ok) {
            printf("  allow \"%s\" %s: got %d\n", allow[i].allow, allow[i].ip, ok);
            all = false;
        }
    }
    check("CLU_TELNET_ALLOW: any / host / subnet; invalid -> refuse all", all);

    // ---- config limits
    free(s_buf);
    free(s_cli);
    clu_telnet_cfg_t small = {.port = 23, .max_clients = 0, .history = 10};
    clu_telnet_init(&small);
    check("config: at least 1 client and 256 bytes of history", s_cfg.max_clients == 1 && s_size == 256);
    free(s_buf);
    free(s_cli);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
