// Host tests for the Lua runtime (clu_lua.c, included: bindings, allocator and VM lifecycle are
// static). The real prelude.lua / objects.lua are embedded like in the firmware.
//
// Part 1 calls the bindings, the class-0 (local CLU) object, the PSRAM allocator, VM creation and
// request execution directly. Part 2 runs the Lua task on a real thread (FreeRTOS queues and
// semaphores re-implemented on pthreads) with MAIN.LUA / EMERGNCY.LUA files in a temporary
// directory: req: calls, driver events, remote replies, soft reload, emergency mode and halt.
#define _GNU_SOURCE
#include <errno.h>
#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "freertos/semphr.h"

// SetDateTime must not touch the host clock.
static time_t s_settime = -1;
static int host_settimeofday(const struct timeval *tv, const void *tz)
{
    s_settime = tv->tv_sec;
    return 0;
}
#define settimeofday host_settimeofday
static int s_time_sets;
static time_t s_time_set_seen;
static void cb_time_set(void) { s_time_sets++; s_time_set_seen = s_settime; }

// Lua heap budget is a variable here, so a test can shrink it.
static int s_mem_limit_kb = 4096;
#define CONFIG_CLU_LUA_MEM_LIMIT_KB         s_mem_limit_kb
#define CONFIG_CLU_LUA_INTERNAL_RESERVE_KB  64

// Binary semaphores with real blocking (the stub's single shared flag cannot hold per-request
// semaphores across threads).
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int given;
} host_sem_t;
static void *host_sem_new(void);
static BaseType_t host_sem_take(void *s, TickType_t ms);
static BaseType_t host_sem_give(void *s);
static void host_sem_del(void *s);
#define xSemaphoreCreateBinary() host_sem_new()
#define xSemaphoreTake(s, t)     host_sem_take(s, t)
#define xSemaphoreGive(s)        host_sem_give(s)
#define vSemaphoreDelete(s)      host_sem_del(s)

#include "../../components/clu_lua/clu_lua.c"

// The firmware embeds prelude.lua / objects.lua (EMBED_TXTFILES: NUL-terminated).
__asm__(".pushsection .rodata\n"
        ".global _binary_prelude_lua_start\n_binary_prelude_lua_start:\n"
        ".incbin \"../../components/clu_lua/prelude.lua\"\n.byte 0\n"
        ".global _binary_prelude_lua_end\n_binary_prelude_lua_end:\n"
        ".global _binary_objects_lua_start\n_binary_objects_lua_start:\n"
        ".incbin \"../../components/clu_lua/objects.lua\"\n.byte 0\n"
        ".global _binary_objects_lua_end\n_binary_objects_lua_end:\n"
        ".popsection\n");

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    fflush(stdout);
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

// ---------------------------------------------------------------- FreeRTOS on pthreads
static void deadline(struct timespec *ts, TickType_t ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

// Wait on c until pred holds; false on timeout. Called with m locked.
#define WAIT_UNTIL(pred, m, c, ms)                                                   \
    ({                                                                               \
        bool ok_ = true;                                                             \
        struct timespec ts_;                                                         \
        deadline(&ts_, (ms));                                                        \
        while (!(pred)) {                                                            \
            if ((ms) == 0) { ok_ = false; break; }                                  \
            if ((ms) == portMAX_DELAY) { pthread_cond_wait(c, m); continue; }        \
            if (pthread_cond_timedwait(c, m, &ts_) == ETIMEDOUT && !(pred)) {        \
                ok_ = false;                                                         \
                break;                                                               \
            }                                                                        \
        }                                                                            \
        ok_;                                                                         \
    })

static void *host_sem_new(void)
{
    host_sem_t *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    return s;
}
static BaseType_t host_sem_take(void *p, TickType_t ms)
{
    host_sem_t *s = p;
    pthread_mutex_lock(&s->m);
    bool ok = WAIT_UNTIL(s->given, &s->m, &s->c, ms);
    if (ok) {
        s->given = 0;
    }
    pthread_mutex_unlock(&s->m);
    return ok ? pdTRUE : pdFALSE;
}
static BaseType_t host_sem_give(void *p)
{
    host_sem_t *s = p;
    pthread_mutex_lock(&s->m);
    s->given = 1;
    pthread_cond_broadcast(&s->c);
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}
static void host_sem_del(void *p)
{
    host_sem_t *s = p;
    pthread_mutex_destroy(&s->m);
    pthread_cond_destroy(&s->c);
    free(s);
}

struct host_queue {
    pthread_mutex_t m;
    pthread_cond_t c;
    size_t item, cap, head, n;
    uint8_t *buf;
};
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item)
{
    QueueHandle_t q = calloc(1, sizeof *q);
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->c, NULL);
    q->item = item;
    q->cap = len;
    q->buf = calloc(len, item);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ms)
{
    pthread_mutex_lock(&q->m);
    bool ok = WAIT_UNTIL(q->n < q->cap, &q->m, &q->c, ms);
    if (ok) {
        memcpy(q->buf + ((q->head + q->n) % q->cap) * q->item, item, q->item);
        q->n++;
        pthread_cond_broadcast(&q->c);
    }
    pthread_mutex_unlock(&q->m);
    return ok ? pdTRUE : pdFALSE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ms)
{
    pthread_mutex_lock(&q->m);
    bool ok = WAIT_UNTIL(q->n > 0, &q->m, &q->c, ms);
    if (ok) {
        memcpy(item, q->buf + q->head * q->item, q->item);
        q->head = (q->head + 1) % q->cap;
        q->n--;
        pthread_cond_broadcast(&q->c);
    }
    pthread_mutex_unlock(&q->m);
    return ok ? pdTRUE : pdFALSE;
}
BaseType_t xQueueReset(QueueHandle_t q)
{
    pthread_mutex_lock(&q->m);
    q->head = q->n = 0;
    pthread_cond_broadcast(&q->c);
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}

static void sleep_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}
void vTaskDelay(TickType_t t) { sleep_ms((long)t); }
static volatile int s_suspended;
void vTaskSuspend(TaskHandle_t t)
{
    s_suspended = 1;
    for (;;) {
        pause();
    }
}
static void *task_trampoline(void *arg)
{
    ((void (*)(void *))arg)(NULL);
    return NULL;
}
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *n, uint32_t s, void *a, UBaseType_t p,
                                   TaskHandle_t *h, BaseType_t c)
{
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 8u << 20);
    int r = pthread_create(&th, &at, task_trampoline, (void *)fn);
    pthread_attr_destroy(&at);
    return r == 0 ? pdTRUE : pdFALSE;
}

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------- host callbacks
static char s_reported[128];
static int s_inits, s_reloads, s_cloud = 1;
static uint32_t s_remote_ip, s_remote_id = 7;
static char s_remote_script[64];
static char s_feature[64];
static void cb_report(const char *addr, int port, const char *body)
{
    snprintf(s_reported, sizeof s_reported, "%s:%d:%s", addr, port, body);
}
static void cb_init(void) { s_inits++; }
static uint32_t cb_remote(uint32_t ip, const char *script)
{
    s_remote_ip = ip;
    snprintf(s_remote_script, sizeof s_remote_script, "%s", script);
    return s_remote_id;
}
static void cb_feature(int idx, const char *str, double num)
{
    snprintf(s_feature, sizeof s_feature, "%d:%s:%g", idx, str ? str : "-", num);
}
static bool cb_cloud(void) { return s_cloud; }
static bool cb_reload(void)
{
    s_reloads++;
    return true;
}

// ---------------------------------------------------------------- files
static char s_root[64];

static void put(const char *name, const char *content)
{
    char p[128];
    snprintf(p, sizeof p, "%s/%s", s_root, name);
    FILE *f = fopen(p, "w");
    fputs(content, f);
    fclose(f);
}

static bool exists(const char *name)
{
    char p[128];
    snprintf(p, sizeof p, "%s/%s", s_root, name);
    return access(p, F_OK) == 0;
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) { return remove(path); }

// ---------------------------------------------------------------- part 1 helpers
static int s_ops_calls;
static char s_ops_log[128];
static void t_on_new(lua_State *L, int obj) { s_ops_calls++; snprintf(s_ops_log, sizeof s_ops_log, "new:%s", clu_obj_name(L, obj)); }
static bool t_get(lua_State *L, int obj, int idx)
{
    if (idx == 1) {
        lua_pushinteger(L, 11);
        return true;
    }
    return false;
}
static bool t_set(lua_State *L, int obj, int idx, int val) { return idx == 1; }
static int t_exec(lua_State *L, int obj, int idx, int first, int nargs)
{
    if (idx == 1) {
        lua_pushinteger(L, nargs);
        lua_pushstring(L, "r");
        return 2;
    }
    return -1;
}
static const clu_class_ops_t s_test_ops = {.on_new = t_on_new, .get = t_get, .set = t_set, .execute = t_exec};

// Push an object table {_class = cls, _name = name}; returns its index.
static int obj(lua_State *L, int cls, const char *name)
{
    lua_newtable(L);
    lua_pushinteger(L, cls);
    lua_setfield(L, -2, "_class");
    if (name) {
        lua_pushstring(L, name);
        lua_setfield(L, -2, "_name");
    }
    lua_newtable(L);
    lua_setfield(L, -2, "_f");
    return lua_gettop(L);
}

// Call a lua_CFunction with the given stack (already pushed); returns the result count.
static int ccall(lua_State *L, lua_CFunction fn, int nargs)
{
    lua_pushcfunction(L, fn);
    lua_insert(L, -1 - nargs);
    int top = lua_gettop(L) - nargs - 1;
    if (lua_pcall(L, nargs, LUA_MULTRET, 0) != LUA_OK) {
        return -1; // error message on the stack
    }
    return lua_gettop(L) - top;
}

static req_t *mkreq(const char *expr)
{
    req_t *r = calloc(1, sizeof *r);
    r->expr = strdup(expr);
    r->cap = 64;
    r->out = calloc(1, r->cap);
    r->done = xSemaphoreCreateBinary();
    snprintf(r->from, sizeof r->from, "10.9.8.7");
    return r;
}

static const char *run_req(lua_State *L, const char *expr)
{
    static char out[64];
    req_t *r = mkreq(expr);
    exec_request(L, r);
    snprintf(out, sizeof out, "%s", r->out);
    check("req: completion signalled", xSemaphoreTake(r->done, 0) == pdTRUE);
    req_free(r);
    return out;
}

static void dummy_hook(lua_State *L, lua_Debug *ar) {}

static int ext_fn(lua_State *L)
{
    lua_pushstring(L, "ext");
    return 1;
}

// ---------------------------------------------------------------- part 1
static void test_unit(void)
{
    char out[64];
    check("call before start -> invalid state", clu_lua_call("1", "", out, sizeof out, 10) == ESP_ERR_INVALID_STATE);
    clu_lua_post_event("X", 1);
    clu_lua_post_number("X", 1, 2);
    clu_lua_post_string("X", 1, "s");
    clu_lua_post_remote(1, "r");
    check("reload / halt before start -> false", !clu_lua_reload(10) && !clu_lua_halt(10));
    s_halt = false;

    static const luaL_Reg ext_fns[] = {{"ext_fn", ext_fn}, {NULL, NULL}};
    static const char ext_src[] = "EXT_LOADED = __clu.ext_fn() == 'ext'";
    clu_lua_register_ext(ext_fns, ext_src, sizeof ext_src - 1, "=ext");
    clu_lua_register_ext(NULL, "this is not lua", 15, "=broken");
    clu_lua_register_ext(ext_fns, NULL, 0, "=fns_only");
    clu_lua_register_ext(NULL, "EXT4 = true", 11, "=ext4");
    clu_lua_register_ext(NULL, "EXT5 = true", 11, "=ext5");
    clu_lua_register_ext(NULL, "EXT6 = true", 11, "=ext6");
    clu_lua_register_ext(NULL, "EXT7 = true", 11, "=ext7");
    check("at most 6 extensions", s_n_exts == MAX_EXTS && MAX_EXTS == 6);
    clu_lua_register_class(-1, &s_test_ops);
    clu_lua_register_class(MAX_CLASSES, &s_test_ops);
    clu_lua_register_class(50, &s_test_ops);
    clu_lua_register_class(0, &s_clu0_ops);

    s_cfg = (clu_lua_cfg_t){.root = s_root, .sn = 0x13aba9b3, .fw_version = "1.2.3", .report = cb_report,
                            .on_init = cb_init, .remote = cb_remote, .clu_feature = cb_feature,
                            .cloud_connected = cb_cloud};

    // ---- VM creation
    lua_State *L = vm_new();
    check("vm: created with prelude + objects", L != NULL);
    check_str("vm: default checkAlive() returns the serial", run_req(L, "checkAlive()"), "13aba9b3");
    check_str("vm: __clu hidden from scripts", run_req(L, "type(__clu)"), "nil");
    check_str("vm: extension functions + source loaded, broken one skipped", run_req(L, "EXT_LOADED and EXT4"), "true");
    check_str("vm: 7th extension ignored", run_req(L, "EXT7"), "nil");

    // ---- req: execution
    check_str("req: expression", run_req(L, "1 + 1\r\n"), "2");
    check_str("req: statement -> nil", run_req(L, "x = 5"), "nil");
    check_str("req: state kept between calls", run_req(L, "x * 2"), "10");
    check_str("req: caller IP in __req_from", run_req(L, "__req_from"), "10.9.8.7");
    check_str("req: runtime error -> nil", run_req(L, "error('boom')"), "nil");
    check_str("req: syntax error -> nil", run_req(L, "1 +"), "nil");
    check_str("req: non-string error -> nil", run_req(L, "error({})"), "nil");
    lua_sethook(L, dummy_hook, LUA_MASKCOUNT, 77);
    int64_t t0 = esp_timer_get_time();
    check_str("req: runaway script aborted -> nil", run_req(L, "(function() while true do end end)()"), "nil");
    check("req: runaway aborted after the 2.5 s budget",
          esp_timer_get_time() - t0 >= REQ_BUDGET_MS * 1000LL && esp_timer_get_time() - t0 < 4000000);
    check("req: caller's hook restored", lua_gethook(L) == dummy_hook && lua_gethookcount(L) == 77);
    lua_sethook(L, NULL, 0, 0);
    {
        req_t *r = mkreq("1");
        r->state = REQ_ABANDONED; // caller timed out: the Lua side frees it (ASan checks)
        exec_request(L, r);
        check("req: abandoned request freed by the runtime", 1);
    }
    lua_sethook(L, req_hook, LUA_MASKCOUNT, 1);
    s_req_active = false;
    req_hook(L, NULL);
    check("req hook outside a req: (resumed coroutine) removes itself", lua_gethook(L) == NULL);
    lua_settop(L, 0);

    // ---- memory budget
    lua_gc(L, LUA_GCCOLLECT, 0);
    int saved = s_mem_limit_kb;
    s_mem_limit_kb = (int)(s_mem.used / 1024) + 64;
    check_str("heap budget: allocation past the limit -> 'not enough memory'",
              run_req(L, "select(2, pcall(string.rep, 'x', 1 << 20)):match('memory') and 'oom'"), "oom");
    s_mem_limit_kb = saved;
    vm_close(L);

    // ---- allocator details
    lua_mem_t m = {0};
    void *p = psram_alloc(&m, NULL, LUA_TSTRING, 100);
    check("alloc: new block counted", p && m.used == 100 && m.peak == 100);
    p = psram_alloc(&m, p, 100, 40);
    check("alloc: shrink", p && m.used == 40 && m.peak == 100);
    host_heap_fail = 1;
    p = psram_alloc(&m, p, 40, 400);
    check("alloc: PSRAM full -> internal heap (reserve kept)", p && m.used == 400);
    host_heap_fail = 1;
    void *q = psram_alloc(&m, NULL, 0, 150000);
    check("alloc: PSRAM full and internal reserve would be hit -> NULL", !q && m.used == 400);
    host_heap_fail = 0;
    s_mem_limit_kb = 1;
    check("alloc: growth past the budget -> NULL", !psram_alloc(&m, p, 400, 2000) && m.used == 400);
    s_mem_limit_kb = saved;
    check("alloc: free", !psram_alloc(&m, p, 400, 0) && m.used == 0);

    // ---- emergency VM, broken embedded chunk
    s_emergency = true;
    L = vm_new();
    check_str("emergency vm: checkAlive() -> emergency", run_req(L, "checkAlive()"), "emergency");
    vm_close(L);
    s_emergency = false;
    L = luaL_newstate();
    static const char bad[] = "x = = 1";
    check("broken embedded chunk -> false", !load_embedded(L, bad, bad + sizeof bad, "=bad"));

    // ---- object hooks
    int o = obj(L, 50, "OBJ");
    check("new -> class on_new", ccall(L, l_new, (lua_pushvalue(L, o), 1)) == 0 && !strcmp(s_ops_log, "new:OBJ"));
    lua_pushvalue(L, o);
    lua_pushinteger(L, 1);
    int n = ccall(L, l_get, 2);
    check("get handled -> true, value", n == 2 && lua_toboolean(L, -2) && lua_tointeger(L, -1) == 11);
    lua_settop(L, o);
    lua_pushvalue(L, o);
    lua_pushinteger(L, 2);
    n = ccall(L, l_get, 2);
    check("get not handled -> false", n == 1 && !lua_toboolean(L, -1));
    lua_settop(L, o);
    lua_pushvalue(L, o);
    lua_pushinteger(L, 1);
    lua_pushinteger(L, 5);
    n = ccall(L, l_set, 3);
    check("set handled -> true", n == 1 && lua_toboolean(L, -1));
    lua_settop(L, o);
    lua_pushvalue(L, o);
    lua_pushinteger(L, 1);
    lua_pushinteger(L, 8);
    lua_pushinteger(L, 9);
    n = ccall(L, l_execute, 4);
    check("execute: results of the class hook", n == 2 && lua_tointeger(L, -2) == 2 && !strcmp(lua_tostring(L, -1), "r"));
    lua_settop(L, o);
    lua_pushvalue(L, o);
    lua_pushinteger(L, 9);
    check("execute: unknown method -> no results", ccall(L, l_execute, 2) == 0);
    lua_settop(L, o);
    int none = obj(L, 99, NULL); // class without hooks, no name
    lua_pushvalue(L, none);
    lua_pushinteger(L, 0);
    check("get / execute on a class without hooks", ccall(L, l_get, 2) == 1 && !lua_toboolean(L, -1));
    lua_settop(L, none);
    lua_pushvalue(L, none);
    lua_pushinteger(L, 0);
    check("execute on a class without hooks", ccall(L, l_execute, 2) == 0 && !strcmp(clu_obj_name(L, none), ""));
    lua_pushvalue(L, none);
    lua_pushinteger(L, 0);
    lua_pushnil(L);
    check("set on a class without hooks -> false", ccall(L, l_set, 3) == 1 && !lua_toboolean(L, -1));
    lua_pushvalue(L, none);
    check("new on a class without hooks", ccall(L, l_new, 1) == 0);
    lua_pushinteger(L, 3);
    check("get on a non-table -> error", ccall(L, l_get, 1) == -1);
    lua_settop(L, 0);

    // ---- log / report / millis / wait / remote / init
    for (int lvl = 1; lvl <= 4; lvl++) {
        lua_pushinteger(L, lvl);
        lua_pushstring(L, "msg");
        lua_pushnil(L);
        ccall(L, l_log, 3);
    }
    check("log: all levels, several values", lua_gettop(L) == 0);
    lua_pushstring(L, "192.168.1.9");
    lua_pushinteger(L, 5000);
    lua_pushstring(L, "body");
    ccall(L, l_report, 3);
    check_str("report -> host callback", s_reported, "192.168.1.9:5000:body");
    lua_settop(L, 0);
    n = ccall(L, l_millis, 0);
    check("millis: monotonic ms", n == 1 && lua_tointeger(L, -1) > 0);
    lua_settop(L, 0);
    lua_pushinteger(L, 5);
    int64_t w0 = esp_timer_get_time();
    ccall(L, l_wait, 1);
    check("wait(5) sleeps", esp_timer_get_time() - w0 >= 5000);
    ccall(L, l_wait, 0);

    lua_pushstring(L, "10.0.0.5");
    lua_pushstring(L, "DOU1:get(0)");
    n = ccall(L, l_remote, 2);
    check("remote(\"a.b.c.d\", script) -> transport, call id",
          n == 1 && lua_tointeger(L, -1) == 7 && s_remote_ip == 0x0a000005 && !strcmp(s_remote_script, "DOU1:get(0)"));
    lua_settop(L, 0);
    lua_pushinteger(L, 0x0a000006);
    lua_pushstring(L, "x");
    ccall(L, l_remote, 2);
    check("remote(ip number, ...)", s_remote_ip == 0x0a000006);
    lua_settop(L, 0);
    lua_pushstring(L, "300.1.1.1");
    lua_pushstring(L, "x");
    check("remote: bad address -> error", ccall(L, l_remote, 2) == -1);
    lua_settop(L, 0);
    s_remote_id = 0;
    lua_pushstring(L, "10.0.0.5");
    lua_pushstring(L, "x");
    check("remote: transport refused -> nil", ccall(L, l_remote, 2) == 0);
    s_remote_id = 7;
    s_cfg.remote = NULL;
    lua_pushstring(L, "10.0.0.5");
    lua_pushstring(L, "x");
    check("remote: no transport -> nil", ccall(L, l_remote, 2) == 0);
    s_cfg.remote = cb_remote;

    ccall(L, l_init, 0);
    check("init: state OK, host on_init", s_state == CLU_LUA_OK && s_inits == 1);
    s_emergency = true;
    ccall(L, l_init, 0);
    check("init in emergency mode: state EMERGENCY", s_state == CLU_LUA_EMERGENCY);
    s_emergency = false;
    s_cfg.on_init = NULL;
    ccall(L, l_init, 0);
    s_cfg.on_init = cb_init;
    s_cfg.report = NULL;
    lua_pushstring(L, "a");
    lua_pushinteger(L, 1);
    lua_pushstring(L, "b");
    ccall(L, l_report, 3);
    s_cfg.report = cb_report;
    lua_settop(L, 0);

    // ---- class 0: local CLU
    int clu = obj(L, 0, "CLU");
    s_boot_us = esp_timer_get_time() - 5000000;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    struct { int idx; lua_Integer want; } ints[] = {
        {0, 5}, {2, CLU_LUA_OK}, {3, 1}, {7, tm.tm_mday}, {8, tm.tm_mon + 1}, {9, tm.tm_year + 1900},
        {10, tm.tm_wday ? tm.tm_wday : 7}, {11, tm.tm_hour}, {18, 1}, {19, 1},
    };
    s_state = CLU_LUA_OK;
    bool all = true;
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++) {
        bool h = clu0_get(L, clu, ints[i].idx);
        all = all && h && lua_tointeger(L, -1) == ints[i].want;
        if (!(h && lua_tointeger(L, -1) == ints[i].want)) {
            printf("  get(%d) = %lld, want %lld\n", ints[i].idx, (long long)lua_tointeger(L, -1), (long long)ints[i].want);
        }
        lua_settop(L, clu);
    }
    check("CLU get: Uptime, State, IsLocalPower, date parts, ISO weekday, hour, UseCloud default, CloudConnection", all);
    char date[16], tod[16];
    snprintf(date, sizeof date, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    clu0_get(L, clu, 5);
    clu0_get(L, clu, 6);
    clu0_get(L, clu, 12);
    clu0_get(L, clu, 13);
    clu0_get(L, clu, 17);
    check("CLU get: Date, Time, minute, epoch, firmware version",
          !strcmp(lua_tostring(L, clu + 1), date) && strlen(lua_tostring(L, clu + 2)) == 8 &&
              lua_tointeger(L, clu + 4) >= now && !strcmp(lua_tostring(L, clu + 5), "1.2.3"));
    (void)tod;
    lua_settop(L, clu);
    lua_getfield(L, clu, "_f");
    lua_pushinteger(L, 0);
    lua_rawseti(L, -2, 18);
    lua_settop(L, clu);
    clu0_get(L, clu, 18);
    s_cloud = 0;
    clu0_get(L, clu, 19);
    s_cfg.cloud_connected = NULL;
    clu0_get(L, clu, 19);
    s_cfg.fw_version = NULL;
    clu0_get(L, clu, 17);
    check("CLU get: UseCloud stored, CloudConnection down / unknown, no fw version",
          lua_tointeger(L, clu + 1) == 0 && lua_tointeger(L, clu + 2) == 0 && lua_tointeger(L, clu + 3) == 0 &&
              !strcmp(lua_tostring(L, clu + 4), ""));
    s_cfg.fw_version = "1.2.3";
    s_cfg.cloud_connected = cb_cloud;
    check("CLU get: unknown feature not handled", !clu0_get(L, clu, 99));
    lua_settop(L, clu);
    check("CLU get: LastMeasurementSendTime (fwApiVer 0x203) empty, MeasurementKey stored generically",
          clu0_get(L, clu, 35) && !strcmp(lua_tostring(L, -1), "") && !clu0_get(L, clu, 34));
    lua_settop(L, clu);

    // GATE (clu_GATE_HTTP interface): other feature indices
    s_cfg.gate = true;
    s_cfg.reset_reason = 3;
    lua_getfield(L, clu, "_f");
    lua_pushnil(L);
    lua_rawseti(L, -2, 18);
    lua_settop(L, clu);
    s_cloud = 1;
    bool g_ok = clu0_get(L, clu, 0) && lua_tointeger(L, -1) == 5;
    g_ok = g_ok && clu0_get(L, clu, 5) && !strcmp(lua_tostring(L, -1), date);
    g_ok = g_ok && clu0_get(L, clu, 6) && strlen(lua_tostring(L, -1)) == 8;
    g_ok = g_ok && clu0_get(L, clu, 13) && lua_tointeger(L, -1) >= now;
    g_ok = g_ok && clu0_get(L, clu, 15) && lua_tointeger(L, -1) >= now;
    g_ok = g_ok && clu0_get(L, clu, 17) && !strcmp(lua_tostring(L, -1), "1.2.3");
    g_ok = g_ok && clu0_get(L, clu, 18) && lua_tointeger(L, -1) == 1;
    g_ok = g_ok && clu0_get(L, clu, 19) && lua_tointeger(L, -1) == 1;
    g_ok = g_ok && clu0_get(L, clu, 26) && lua_tointeger(L, -1) == 3;
    check("GATE get: Uptime, Date, Time, LocalTime, UnixTime, FirmwareVersion, UseCloud, CloudConnection, ResetReason", g_ok);
    lua_settop(L, clu);
    check("GATE get: CLU-only and settable features stored generically (State 2 = PrimaryDNS, 14 TimeZone, 35)",
          !clu0_get(L, clu, 2) && !clu0_get(L, clu, 3) && !clu0_get(L, clu, 14) && !clu0_get(L, clu, 35) &&
              !clu0_get(L, clu, 1));
    s_soft_reloaded = true;
    check("GATE ResetReason after a soft reset: 2", clu0_get(L, clu, 26) && lua_tointeger(L, -1) == 2);
    s_soft_reloaded = false;
    s_cfg.gate = false;
    s_cfg.reset_reason = 0;
    lua_settop(L, clu);

    lua_pushstring(L, "hello");
    check("CLU AddToLog(msg)", clu0_execute(L, clu, 0, clu + 1, 1) == 0);
    check("CLU AddToLog() / ClearLog", clu0_execute(L, clu, 0, clu + 1, 0) == 0 && clu0_execute(L, clu, 1, clu + 1, 0) == 0);
    lua_settop(L, clu);
    lua_pushinteger(L, 1700000000);
    check("CLU SetDateTime(epoch)", clu0_execute(L, clu, 2, clu + 1, 1) == 0 && s_settime == 1700000000);
    s_cfg.on_time_set = cb_time_set;
    s_time_sets = 0;
    lua_settop(L, clu);
    lua_pushinteger(L, 1700000001);
    check("CLU SetDateTime(epoch): host told after the clock is set (RTC write-back)",
          clu0_execute(L, clu, 2, clu + 1, 1) == 0 && s_time_sets == 1 && s_time_set_seen == 1700000001);
    s_settime = -1;
    check("CLU SetDateTime() without argument: clock untouched, host not told",
          clu0_execute(L, clu, 2, clu + 1, 0) == 0 && s_settime == -1 && s_time_sets == 1);
    s_cfg.on_time_set = NULL;
    check("CLU unknown method -> -1", clu0_execute(L, clu, 9, clu + 1, 0) == -1);
    lua_settop(L, clu);
    lua_pushstring(L, "pool.ntp.org");
    clu0_set(L, clu, 14, clu + 1);
    check_str("CLU set(string) -> host feature callback", s_feature, "14:pool.ntp.org:0");
    lua_settop(L, clu);
    lua_pushinteger(L, 3);
    check("CLU set(number) not handled (stored generically)", !clu0_set(L, clu, 15, clu + 1));
    check_str("CLU set(number) -> host feature callback", s_feature, "15:-:3");
    s_cfg.clu_feature = NULL;
    clu0_set(L, clu, 15, clu + 1);
    s_cfg.clu_feature = cb_feature;
    // GATE: UseCloud = 1 / true also switches UseNTP (21) on; UseCloud = 0 leaves it alone
    s_cfg.gate = true;
    lua_settop(L, clu);
    lua_getfield(L, clu, "_f");
    lua_pushinteger(L, 0);
    lua_rawseti(L, -2, 21);
    lua_settop(L, clu);
    lua_pushinteger(L, 0);
    clu0_set(L, clu, 18, clu + 1);
    lua_getfield(L, clu, "_f");
    lua_rawgeti(L, -1, 21);
    bool ntp_kept = lua_tointeger(L, -1) == 0 && !strcmp(s_feature, "18:-:0");
    lua_settop(L, clu);
    lua_pushboolean(L, 1);
    clu0_set(L, clu, 18, clu + 1);
    lua_getfield(L, clu, "_f");
    lua_rawgeti(L, -1, 21);
    check("GATE UseCloud on -> UseNTP stored 1 and applied; off -> UseNTP untouched",
          ntp_kept && lua_tointeger(L, -1) == 1 && !strcmp(s_feature, "21:-:1"));
    s_cfg.gate = false;
    lua_close(L);

    // ---- shutdown hooks
    L = luaL_newstate();
    luaL_openlibs(L);
    (void)luaL_dostring(L, "function __shutdown() error('x') end");
    clu_lua_shutdown_hooks(L);
    check("shutdown hook error: logged, stack kept", lua_gettop(L) == 0);
    lua_close(L);
}

// ---------------------------------------------------------------- part 2: Lua task
static const char *call(const char *expr, int timeout_ms, esp_err_t *err)
{
    static char out[128];
    out[0] = 0;
    esp_err_t e = clu_lua_call(expr, "10.1.2.3", out, sizeof out, timeout_ms);
    if (err) {
        *err = e;
    }
    return e == ESP_OK ? out : NULL;
}

static bool wait_state(clu_lua_state_t st)
{
    for (int i = 0; i < 300 && clu_lua_state() != st; i++) {
        sleep_ms(10);
    }
    return clu_lua_state() == st;
}

static const char *MAIN1 =
    "evs = {}\n"
    "function __fire(n, i) if n == 'ERR' then error('bad') end evs[#evs + 1] = n .. '!' .. i end\n"
    "function __update(n, i, v) evs[#evs + 1] = n .. '[' .. i .. ']=' .. tostring(v) .. ':' .. (math.type(v) or type(v)) end\n"
    "function __remote_done(id, r) if id == 99 then error('x') end evs[#evs + 1] = 'R' .. id .. '=' .. tostring(r) end\n"
    "function takeev() local s = table.concat(evs, ' ') evs = {} return s end\n"
    "function busy(s) local t = os.clock() while os.clock() - t < s do end return 'done' end\n"
    "function checkAlive() return 'main1' end\n"
    "SYSTEM.Init()\n"
    "SYSTEM.Wait(nil, 1)\n"
    "repeat SYSTEM.Loop() until false\n";

static void test_task(void)
{
    put("MAIN.LUA", MAIN1);
    clu_lua_cfg_t cfg = {.root = s_root, .sn = 0x13aba9b3, .fw_version = "1.2.3", .report = cb_report,
                         .on_init = cb_init, .remote = cb_remote, .clu_feature = cb_feature,
                         .cloud_connected = cb_cloud, .on_reload = cb_reload};
    s_inits = 0;
    s_state = CLU_LUA_STARTING;
    clu_lua_start(&cfg);
    check("task: MAIN.LUA ran SYSTEM.Init -> state OK", wait_state(CLU_LUA_OK) && s_inits == 1);
    check_str("task: req: answered from MAIN.LUA's loop", call("checkAlive()", 3000, NULL), "main1");
    check_str("task: caller IP visible", call("__req_from", 3000, NULL), "10.1.2.3");

    clu_lua_post_event("A", 3);
    clu_lua_post_number("B", 1, 2.5);
    clu_lua_post_number("C", 0, 7);
    clu_lua_post_string("D", 6, "#FF8000");
    clu_lua_post_event("ERR", 1);
    clu_lua_post_remote(5, "ok");
    clu_lua_post_remote(6, NULL);
    clu_lua_post_remote(99, "x");
    sleep_ms(100);
    check_str("task: driver events, value updates (int / float / string) and remote replies delivered",
              call("takeev()", 3000, NULL),
              "A!3 B[1]=2.5:float C[0]=7:integer D[6]=#FF8000:string R5=ok R6=nil");

    esp_err_t e;
    check("task: caller timeout while the script is busy -> ESP_ERR_TIMEOUT",
          !call("busy(0.4)", 50, &e) && e == ESP_ERR_TIMEOUT);
    sleep_ms(600);
    check_str("task: runtime still answers after an abandoned request", call("1 + 2", 3000, NULL), "3");

    // ---- soft reload with a new MAIN.LUA
    put("MAIN.LUA", "function checkAlive() return 'main2' end\nSYSTEM.Init()\nrepeat SYSTEM.Loop() until false\n");
    check("reload: completed, host on_reload called", clu_lua_reload(5000) && s_reloads == 1);
    check_str("reload: new MAIN.LUA running", call("checkAlive()", 3000, NULL), "main2");

    // ---- MAIN.LUA that returns: the runtime keeps looping itself
    put("MAIN.LUA", "function checkAlive() return 'main3' end\n");
    check("reload 2", clu_lua_reload(5000));
    check_str("MAIN.LUA returned: requests still served by the idle loop", call("checkAlive()", 3000, NULL), "main3");

    // ---- GATE MAIN.LUA (as uploaded by OM): no loop, checkAlive() only prints
    s_cfg.gate = true;
    put("MAIN.LUA", "\ncollectgarbage(\"collect\")\nfunction checkAlive()\n\tprint(\"true\")\nend\n\n"
                    "SYSTEM.Init()\nGATE_UP = 1\n");
    check("reload to a GATE MAIN.LUA", clu_lua_reload(5000));
    check_str("GATE: checkAlive() answered by the firmware with the serial in hex", call("checkAlive()", 3000, NULL),
              "13aba9b3");
    check_str("GATE: script ran, other requests still evaluated", call("GATE_UP", 3000, NULL), "1");
    s_cfg.gate = false;

    // ---- SYSTEM.Loop failing in the idle loop: reload still gets through
    put("MAIN.LUA", "SYSTEM.Loop = function() error('loop broken') end\n");
    check("reload 3", clu_lua_reload(5000));
    sleep_ms(150);
    put("MAIN.LUA", "function checkAlive() return 'main4' end\n");
    check("reload out of a failing idle loop", clu_lua_reload(5000));
    check_str("after the failing loop", call("checkAlive()", 3000, NULL), "main4");

    // ---- broken MAIN.LUA -> BAD_M.LUA, emergency mode with EMERGNCY.LUA
    put("MAIN.LUA", "error('broken script')\n");
    put("EMERGNCY.LUA", "function checkAlive() return 'emerg' end\n");
    check("reload into a broken MAIN.LUA", clu_lua_reload(5000));
    check("emergency: state EMERGENCY, MAIN.LUA kept as BAD_M.LUA",
          wait_state(CLU_LUA_EMERGENCY) && exists("BAD_M.LUA") && !exists("MAIN.LUA"));
    check_str("emergency: EMERGNCY.LUA answers", call("checkAlive()", 3000, NULL), "emerg");

    // ---- no MAIN.LUA: idle VM with the default checkAlive()
    check("reload without MAIN.LUA", clu_lua_reload(5000));
    check_str("no MAIN.LUA: default checkAlive()", call("checkAlive()", 3000, NULL), "13aba9b3");
    check("no MAIN.LUA: state OK", clu_lua_state() == CLU_LUA_OK);

    // ---- halt (before a reboot): VM closed, nothing answers any more
    check("halt: VM closed", clu_lua_halt(5000));
    sleep_ms(50);
    check("halt: task suspended, requests time out", s_suspended && !call("1", 200, &e) && e == ESP_ERR_TIMEOUT);

    // nothing drains the queues any more: they fill up
    for (int i = 0; i < 16; i++) {
        call("1", 1, NULL);
    }
    check("request queue full -> ESP_ERR_TIMEOUT before queuing", !call("1", 5, &e) && e == ESP_ERR_TIMEOUT &&
                                                                       s_reqs->n == 16);
    for (int i = 0; i < 17; i++) {
        clu_lua_post_remote((uint32_t)i, "r");
    }
    check("remote reply queue full: reply dropped (freed)", s_remote_res->n == 16);
}

int main(void)
{
    snprintf(s_root, sizeof s_root, "/tmp/clu_lua_XXXXXX");
    if (!mkdtemp(s_root)) {
        perror("mkdtemp");
        return 1;
    }
    test_unit();
    test_task();
    if (strncmp(s_root, "/tmp/clu_lua_", 13) == 0) {
        nftw(s_root, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
    }
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    fflush(stdout);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS; // the parked Lua task thread ends with the process
}
