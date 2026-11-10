// Host tests for the TFBUS bus master. tfbus_master.c is included directly (its bus logic is in
// static functions) and run against FreeRTOS stubs and a simulated RS485 bus of modules.
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../components/tfbus_master/tfbus_master.c"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
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
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)wait;
    if (!q->n) {
        return pdFALSE;
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
// The bus task runs synchronously inside the test: run_bus() calls bus_task(), which returns here
// (longjmp) when it would block (ulTaskNotifyTake) or after a budget of polling rounds
// (vTaskDelay(1) ends each round). xTaskNotifyGive() runs it when s_bus_on_notify is set, like the
// real task waking up on tfbus_master_start() / tfbus_master_refresh().
static jmp_buf s_bus_escape;
static bool s_in_bus, s_bus_blocked, s_bus_on_notify;
static int s_rounds_left, s_bus_rounds = 3;
static void run_bus(int rounds)
{
    s_in_bus = true;
    s_bus_blocked = false;
    s_rounds_left = rounds;
    if (setjmp(s_bus_escape) == 0) {
        bus_task(NULL);
    }
    s_in_bus = false;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return s_in_bus ? s_task : NULL; }
void vTaskDelay(TickType_t t)
{
    if (s_in_bus && t == 1 && --s_rounds_left <= 0) {
        longjmp(s_bus_escape, 1);
    }
}
// Run by the bus task's next wait instead of blocking: what another task does meanwhile (e.g.
// tfbus_master_start()), after which the wait returns as if notified.
static void (*s_while_blocked)(void);
uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t w)
{
    (void)c, (void)w;
    if (s_in_bus && s_while_blocked) {
        void (*fn)(void) = s_while_blocked;
        s_while_blocked = NULL;
        fn();
        return 1;
    }
    if (s_in_bus) {
        s_bus_blocked = true;
        longjmp(s_bus_escape, 1);
    }
    return 0;
}
void xTaskNotifyGive(TaskHandle_t t)
{
    (void)t;
    if (s_bus_on_notify && !s_in_bus) {
        run_bus(s_bus_rounds);
    }
}
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *n, uint32_t s, void *a, UBaseType_t p,
                                   TaskHandle_t *h, BaseType_t c)
{
    (void)fn, (void)n, (void)s, (void)a, (void)p, (void)c;
    *h = (TaskHandle_t)0x1; // never run: the test drives discover()/poll_module() itself
    return pdTRUE;
}

// ---------------------------------------------------------------- clu_lua stubs
static const clu_class_ops_t *s_ops[128];
void clu_lua_register_class(int id, const clu_class_ops_t *ops) { s_ops[id] = ops; }
const char *clu_obj_name(lua_State *L, int obj)
{
    lua_getfield(L, obj, "_name");
    const char *s = lua_tostring(L, -1);
    lua_pop(L, 1); // string stays alive: referenced by the object table
    return s ? s : "?";
}
int clu_obj_class(lua_State *L, int obj)
{
    lua_getfield(L, obj, "_class");
    int c = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return c;
}

// ---------------------------------------------------------------- simulated bus
typedef struct {
    uint32_t sn;
    bool present;      // on the bus
    int nack_assign;   // ignore this many ADDR_ASSIGN frames first
    bool corrupt;      // reply with a bad XOR
    uint8_t addr;      // assigned by ADDR_ASSIGN
    uint8_t status[8];
    uint8_t status_len;
    bool discover;     // answers DISCOVER (0x08) while unaddressed
    uint8_t ident[3];  // hwType fwType fwApiVer
    uint8_t ver[4];    // VERSION_QUERY reply, ver_len bytes (0 = no reply)
    uint8_t ver_len;
    bool garble;       // DISCOVER reply with a bad XOR (several modules answering at once)
} sim_mod_t;

static sim_mod_t s_sim[4];
static int s_nsim;
static char s_log[8192];
static size_t s_loglen;

// Frames sent per type (long bus-task runs overflow the text log, which then stops growing).
static int s_tx_count[256];

static void log_frame(const char *dir, const uint8_t *f, size_t n)
{
    s_tx_count[f[2]]++;
    if (s_loglen + strlen(dir) + 2 * n + 2 > sizeof s_log) {
        return;
    }
    s_loglen += sprintf(s_log + s_loglen, "%s", dir);
    for (size_t i = 0; i < n; i++) {
        s_loglen += sprintf(s_log + s_loglen, "%02x", f[i]);
    }
    s_log[s_loglen++] = ' ';
    s_log[s_loglen] = 0;
}

static void log_reset(void)
{
    memset(s_tx_count, 0, sizeof s_tx_count);
    s_loglen = 0;
    s_log[0] = 0;
}

static int count_in_log(const char *needle)
{
    int n = 0;
    for (const char *p = s_log; (p = strstr(p, needle)); p++) {
        n++;
    }
    return n;
}

esp_err_t tfbus_uart_send(const uint8_t *tx, size_t txlen)
{
    log_frame("", tx, txlen);
    return ESP_OK;
}

static int reply(uint8_t *rx, const uint8_t *p, size_t n)
{
    rx[0] = 0xaa;
    rx[1] = 0x00;
    rx[2] = (uint8_t)(n + 1);
    memcpy(rx + 3, p, n);
    rx[3 + n] = tfbus_xor(rx, 3 + n);
    return (int)(4 + n);
}

static int ack(uint8_t *rx)
{
    memcpy(rx, (const uint8_t[]){0xaa, 0x00, 0x01, 0xab}, 4);
    return 4;
}

int tfbus_uart_transact(const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxcap, int timeout_ms)
{
    (void)rxcap, (void)timeout_ms;
    log_frame("", tx, txlen);
    uint8_t dest = tx[1], type = tx[2];
    if (type == TFBUS_T_SEEK) {
        for (int i = 0; i < s_nsim; i++) {
            if (s_sim[i].present) {
                return ack(rx);
            }
        }
        return 0;
    }
    if (type == TFBUS_T_DISCOVER) {
        for (int i = 0; i < s_nsim; i++) {
            sim_mod_t *s = &s_sim[i];
            if (s->present && s->discover && !s->addr) {
                uint8_t p[7] = {(uint8_t)s->sn, (uint8_t)(s->sn >> 8), (uint8_t)(s->sn >> 16),
                                (uint8_t)(s->sn >> 24), s->ident[0], s->ident[1], s->ident[2]};
                int n = reply(rx, p, sizeof p);
                rx[n - 1] ^= s->garble ? 0xff : 0;
                return n;
            }
        }
        return 0;
    }
    if (type == TFBUS_T_ADDR_ASSIGN) {
        uint32_t sn = tx[3] | tx[4] << 8 | tx[5] << 16 | (uint32_t)tx[6] << 24;
        for (int i = 0; i < s_nsim; i++) {
            sim_mod_t *s = &s_sim[i];
            if (s->present && s->sn == sn) {
                if (s->nack_assign > 0) {
                    s->nack_assign--;
                    return 0;
                }
                s->addr = tx[7];
                return ack(rx);
            }
        }
        return 0;
    }
    for (int i = 0; i < s_nsim; i++) {
        sim_mod_t *s = &s_sim[i];
        if (!s->present || !s->addr || s->addr != dest) {
            continue;
        }
        if (type == TFBUS_T_VERSION_QUERY) {
            return s->ver_len ? reply(rx, s->ver, s->ver_len) : 0;
        }
        if (type == TFBUS_T_STATUS_POLL) {
            rx[0] = 0xaa;
            rx[1] = 0x00;
            rx[2] = (uint8_t)(s->status_len + 1);
            memcpy(rx + 3, s->status, s->status_len);
            rx[3 + s->status_len] = tfbus_xor(rx, 3 + s->status_len) ^ (s->corrupt ? 0xff : 0);
            return 4 + s->status_len;
        }
        return ack(rx);
    }
    return 0;
}

// ---------------------------------------------------------------- fake driver (hwType 0x1e)
static char s_drv[2048];
static size_t s_drvlen;
#define DRV(...) (s_drvlen += sprintf(s_drv + s_drvlen, __VA_ARGS__))

static void fd_init(tfbus_module_t *m) { DRV("init:%u ", m->addr); }
static void fd_status(tfbus_module_t *m, const uint8_t *p, size_t n)
{
    DRV("status:%u:%zu:%02x ", m->addr, n, n ? p[0] : 0);
}
static void fd_online(tfbus_module_t *m, bool on) { DRV("%s:%u ", on ? "online" : "offline", m->addr); }
static void fd_io_new(tfbus_module_t *m, int cls, int ch, const char *name)
{
    DRV("new:%u:%d:%d:%s ", m->addr, cls, ch, name);
}
static int fd_exec(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int first, int nargs)
{
    (void)obj;
    DRV("exec:%u:%d:%d:%d:%d ", m->addr, cls, ch, idx, nargs);
    if (idx == 7) {
        luaL_checkinteger(L, first); // raises on a non-number
    } else if (idx == 8) {             // two results
        lua_pushinteger(L, luaL_checkinteger(L, first) * 2);
        lua_pushstring(L, "x");
        return 2;
    } else if (idx == 9) {
        return -1;
    }
    return 0;
}
static bool fd_ioset(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int val)
{
    if (idx == 5) {
        luaL_checkinteger(L, val);
    }
    tfbus_obj_store(L, obj, idx, 42); // obj index valid inside the protected call
    return idx == 1;
}
static void fd_deinit(tfbus_module_t *m) { DRV("deinit:%u:%s ", m->addr, (char *)m->drv_ctx); }
static const tfbus_driver_t s_fake = {
    .hw_type = 0x1e, .name = "FAKE", .init = fd_init, .on_status = fd_status, .on_online = fd_online,
    .io_new = fd_io_new, .io_execute = fd_exec, .io_set = fd_ioset, .deinit = fd_deinit,
};

static void drv_reset(void)
{
    s_drvlen = 0;
    s_drv[0] = 0;
}

static void check_drv(const char *name, const char *want)
{
    int ok = strcmp(s_drv, want) == 0;
    check(name, ok);
    if (!ok) {
        printf("  got  %s\n  want %s\n", s_drv, want);
    }
}

// ---------------------------------------------------------------- Lua object helpers
static lua_State *L;

// OBJECT:new(2, sn, hwType, name) as built by prelude.lua; leaves the table on the stack.
static int lua_module(lua_Integer sn, lua_Integer hw, const char *name)
{
    lua_newtable(L);
    lua_pushinteger(L, 2);
    lua_setfield(L, -2, "_class");
    lua_pushinteger(L, sn);
    lua_setfield(L, -2, "_sn");
    lua_pushinteger(L, hw);
    lua_setfield(L, -2, "_hwType");
    lua_pushstring(L, name);
    lua_setfield(L, -2, "_name");
    int obj = lua_gettop(L);
    s_ops[2]->on_new(L, obj);
    return obj;
}

// OBJECT:new(cls, module, ch, name); module = stack index of the module table (0 = virtual)
static int lua_io(int cls, int module, int ch, const char *name)
{
    lua_newtable(L);
    lua_pushinteger(L, cls);
    lua_setfield(L, -2, "_class");
    if (module) {
        lua_pushvalue(L, module);
        lua_setfield(L, -2, "_module");
        lua_pushinteger(L, ch);
        lua_setfield(L, -2, "_channel");
    }
    lua_pushstring(L, name);
    lua_setfield(L, -2, "_name");
    int obj = lua_gettop(L);
    s_ops[cls]->on_new(L, obj);
    return obj;
}

static int s_scans;
static void count_scan(void) { s_scans++; }

static char s_deferred[1024];
static void defer_fn(tfbus_module_t *m, void *arg, uint32_t tag)
{
    size_t n = strlen(s_deferred);
    snprintf(s_deferred + n, sizeof s_deferred - n, "%s:%s:%u ", m->name, (const char *)arg, (unsigned)tag);
}

// __clu.execute / __clu.set as called from Lua: [obj, arg]
static int err_exec(lua_State *L) { return s_ops[4]->execute(L, 1, 7, 2, 1); }
static int err_set(lua_State *L)
{
    s_ops[4]->set(L, 1, 5, 2);
    return 0;
}

int main(void)
{
    L = luaL_newstate();
    tfbus_driver_register(&s_fake);
    tfbus_master_init();
    check("class 2 and IO classes registered, 44 (Storage) left free",
          s_ops[2] && s_ops[3] && s_ops[127] && !s_ops[44] && !s_ops[0] && !s_ops[1]);

    // ---- registry from OM.LUA
    int A = lua_module(0x13ab6c81, 0x1e, "mm_A");
    int Z = lua_module(0x008500030011LL, 0x008500030011LL, "zwave"); // Z-Wave node, not TFBUS
    int B = lua_module(0x11111111, 0x99, "mm_B");                    // unknown hwType, no driver
    int C = lua_module(0x22222222, 0x1e, "mm_C");
    lua_getfield(L, A, "_addr");
    lua_getfield(L, Z, "_addr");
    lua_getfield(L, C, "_addr");
    check("modules addressed in OM.LUA order, Z-Wave skipped",
          tfbus_master_count() == 3 && lua_tointeger(L, -3) == 1 && lua_isnil(L, -2) && lua_tointeger(L, -1) == 3);
    lua_pop(L, 3);
    check("find_sn / find_name", tfbus_master_find_sn(0x22222222) == tfbus_master_get(2) &&
                                     tfbus_master_find_name("mm_B") == tfbus_master_get(1) &&
                                     !tfbus_master_find_sn(0x00030011) && !tfbus_master_get(3));
    check("driver bound by hwType", tfbus_master_get(0)->drv == &s_fake && tfbus_master_get(1)->drv == NULL);

    // ---- same SN declared again (OBJECT:new(2, ...) via req:): no duplicate entry / address
    int Ad = lua_module(0x13ab6c81, 0x1e, "mm_A2");
    lua_getfield(L, Ad, "_addr");
    check("module redeclared: same entry and address, name follows",
          tfbus_master_count() == 3 && lua_tointeger(L, -1) == 1 && tfbus_master_find_name("mm_A2") == tfbus_master_get(0));
    lua_pop(L, 2);
    strcpy(tfbus_master_get(0)->name, "mm_A");

    // ---- IO objects routed to the module driver
    drv_reset();
    int dou = lua_io(4, A, 2, "DOU2");
    int vt = lua_io(6, 0, 0, "TIM1"); // virtual
    int bio = lua_io(4, B, 0, "DOUB"); // module without driver
    check_drv("io_new routed with class/channel", "new:1:4:2:DOU2 ");
    drv_reset();
    lua_pushinteger(L, 1500);
    int r1 = s_ops[4]->execute(L, dou, 1, lua_gettop(L), 1);
    lua_pop(L, 1);
    check("io execute routed to driver", r1 == 0 && strcmp(s_drv, "exec:1:4:2:1:1 ") == 0);
    check("virtual object / driverless module -> -1 (unknown method)",
          s_ops[6]->execute(L, vt, 0, 0, 0) == -1 && s_ops[4]->execute(L, bio, 0, 0, 0) == -1);
    check("io set without driver hook -> not handled", !s_ops[4]->set(L, bio, 0, bio));

    // driver hooks run under the driver lock in a protected call: results / errors pass through
    {
        lua_newtable(L);
        lua_setfield(L, dou, "_f"); // prelude gives every object its feature table
        int top = lua_gettop(L);
        lua_pushinteger(L, 21);
        int n = s_ops[4]->execute(L, dou, 8, top + 1, 1);
        check("io execute results passed through the protected call",
              n == 2 && lua_gettop(L) == top + 3 && lua_tointeger(L, -2) == 42 &&
              strcmp(lua_tostring(L, -1), "x") == 0 && host_mutex_held == 0);
        lua_settop(L, top);
        lua_pushinteger(L, 1);
        n = s_ops[4]->execute(L, dou, 9, top + 1, 1);
        check("io execute unknown (-1): stack restored", n == -1 && lua_gettop(L) == top + 1);
        lua_settop(L, top);
        lua_pushinteger(L, 7);
        bool h1 = s_ops[4]->set(L, dou, 1, top + 1);
        bool h0 = s_ops[4]->set(L, dou, 2, top + 1);
        lua_getfield(L, dou, "_f");
        lua_rawgeti(L, -1, 2);
        check("io set: handled flag + obj index valid, stack kept",
              h1 && !h0 && lua_tointeger(L, -1) == 42 && lua_gettop(L) == top + 3 && host_mutex_held == 0);
        lua_settop(L, top);
    }
    {   // a Lua error raised by the driver unlocks first, then propagates to the caller
        int top = lua_gettop(L);
        lua_pushcfunction(L, err_exec);
        lua_pushvalue(L, dou);
        lua_pushstring(L, "abc");
        int st = lua_pcall(L, 2, 0, 0);
        bool msg = st == LUA_ERRRUN && strstr(lua_tostring(L, -1), "number expected");
        lua_settop(L, top);
        lua_pushcfunction(L, err_set);
        lua_pushvalue(L, dou);
        lua_pushstring(L, "abc");
        st = lua_pcall(L, 2, 0, 0);
        check("driver Lua error propagates, driver lock released",
              msg && st == LUA_ERRRUN && host_mutex_held == 0);
        lua_settop(L, top);
    }
    lua_settop(L, 0);

    // ---- discovery: A present, B absent, C ignores the first ADDR_ASSIGN
    s_nsim = 2;
    s_sim[0] = (sim_mod_t){.sn = 0x13ab6c81, .present = true, .status = {1, 0x00, 1, 1, 0x05}, .status_len = 5};
    s_sim[1] = (sim_mod_t){.sn = 0x22222222, .present = true, .nack_assign = 1,
                           .status = {1, 0x00, 1, 1, 0x07}, .status_len = 5};
    log_reset();
    discover(false);
    static const char *want_disc = "aaff0752 aaff2a7f aaff085d aaff02816cab130103 "
                                   "aaff02111111110255 aaff02111111110255 aaff02111111110255 "
                                   "aaff02222222220354 aaff02222222220354 aa01f952 aa03f950 ";
    check("discovery sequence: RESET, SEEK, DISCOVER (silent), ADDR_ASSIGN per module (3 tries max), "
          "VERSION_QUERY per assigned module",
          strcmp(s_log, want_disc) == 0);
    if (strcmp(s_log, want_disc) != 0) {
        printf("  got  %s\n", s_log);
    }
    check("assigned flags", tfbus_master_get(0)->assigned && !tfbus_master_get(1)->assigned &&
                                tfbus_master_get(2)->assigned);
    {
        tfbus_device_t d[8];
        int nd = tfbus_master_devices(d, 8);
        check("devices: the 2 answering OM.LUA modules, type from OM.LUA, no version",
              nd == 2 && d[0].sn == 0x13ab6c81 && d[0].hw_type == 0x1e && d[0].fw_type == 2 && d[0].known &&
                  d[0].addr == 1 && !d[0].have_ver && d[0].hw_ver == 1 && d[1].sn == 0x22222222 && d[1].addr == 3);
    }

    // ---- discovery with DISCOVER: emulated panel in OM.LUA (C), unknown new panel, IO module
    // with a VERSION_QUERY reply (A)
    s_nsim = 3;
    s_sim[0].ver_len = 4;
    memcpy(s_sim[0].ver, (const uint8_t[]){0x02, 0x02, 0x04, 0x03}, 4);
    s_sim[1].discover = true;
    memcpy(s_sim[1].ident, (const uint8_t[]){0x03, 0x03, 0x04}, 3);
    s_sim[2] = (sim_mod_t){.sn = 0x0f05f123, .present = true, .discover = true, .ident = {0x03, 0x03, 0x04}};
    for (int i = 0; i < s_nsim; i++) {
        s_sim[i].addr = 0; // RESET
    }
    log_reset();
    discover(false);
    static const char *want_scan = "aaff0752 aaff2a7f "
                                   "aaff085d aaff02222222220354 " // C answers -> its OM.LUA address 3
                                   "aaff085d aaff0223f1050f048b " // new panel -> next free address 4
                                   "aaff085d "                    // silence: done
                                   "aaff02816cab130103 "          // A (no DISCOVER) by SN
                                   "aaff02111111110255 aaff02111111110255 aaff02111111110255 "
                                   "aa01f952 ";                   // VQ for A only (panels fwApiVer 4)
    check("DISCOVER loop: found modules addressed (OM.LUA or next free), then ADDR_ASSIGN for the rest",
          strcmp(s_log, want_scan) == 0);
    if (strcmp(s_log, want_scan) != 0) {
        printf("  got  %s\n", s_log);
    }
    {
        tfbus_device_t d[8];
        int nd = tfbus_master_devices(d, 8);
        check("devices: C + new panel (DISCOVER ident) + A (VQ -> fwApiVer 2, fwVer 2.2.4, hwVer 3)",
              nd == 3 && d[0].sn == 0x22222222 && d[0].hw_type == 3 && d[0].fw_type == 3 && d[0].fw_api_ver == 4 &&
                  d[0].known && d[1].sn == 0x0f05f123 && !d[1].known && d[1].addr == 4 && d[1].hw_type == 3 &&
                  d[2].sn == 0x13ab6c81 && d[2].have_ver && d[2].fw_api_ver == 2 && d[2].hw_ver == 3 &&
                  d[2].ver[1] == 2 && d[2].ver[2] == 4);
    }
    // ---- req_refresh_modules: real CLU order (no SEEK, ADDR_ASSIGN first, then DISCOVER).
    // C misses its first ADDR_ASSIGN (3 tries) and is picked up by DISCOVER; B is absent.
    for (int i = 0; i < s_nsim; i++) {
        s_sim[i].addr = 0; // RESET
    }
    s_sim[1].nack_assign = 3;
    log_reset();
    discover(true);
    static const char *want_refresh = "aaff0752 "                    // RESET, no SEEK
                                      "aaff02816cab130103 "          // A by SN
                                      "aaff02111111110255 aaff02111111110255 aaff02111111110255 " // B absent
                                      "aaff02222222220354 aaff02222222220354 aaff02222222220354 " // C no ACK
                                      "aaff085d aaff02222222220354 " // C answers DISCOVER -> addr 3
                                      "aaff085d aaff0223f1050f048b " // new panel -> next free address 4
                                      "aaff085d "                    // silence
                                      "aaff02111111110255 aaff02111111110255 aaff02111111110255 " // B retried
                                      "aa01f952 ";                   // VQ (panels fwApiVer 4 skipped)
    check("refresh: RESET, ADDR_ASSIGN for OM.LUA modules, DISCOVER for leftovers, retry, VERSION_QUERY",
          strcmp(s_log, want_refresh) == 0);
    if (strcmp(s_log, want_refresh) != 0) {
        printf("  got  %s\n", s_log);
    }
    check("refresh: A, C, new panel addressed", tfbus_master_get(0)->assigned && !tfbus_master_get(1)->assigned &&
                                                     tfbus_master_get(2)->assigned && s_sim[2].addr == 4);
    // ---- refresh where the OM.LUA panel C takes its ADDR_ASSIGN: it never answers DISCOVER, and
    // a SMART_PANEL < fwApiVer 5 does not answer VERSION_QUERY. Its identity must come from the
    // previous scan (03:03:04), not fall back to fwApiVer 0 (OM: no "fv00" device interface).
    for (int i = 0; i < s_nsim; i++) {
        s_sim[i].addr = 0; // RESET
    }
    s_sim[1].nack_assign = 0;
    log_reset();
    discover(true);
    static const char *want_refresh2 = "aaff0752 "
                                       "aaff02816cab130103 "          // A by SN
                                       "aaff02111111110255 aaff02111111110255 aaff02111111110255 " // B absent
                                       "aaff02222222220354 "          // C acks: no DISCOVER from it
                                       "aaff085d aaff0223f1050f048b " // new panel
                                       "aaff085d "
                                       "aaff02111111110255 aaff02111111110255 aaff02111111110255 "
                                       "aa01f952 ";                   // VQ for A only: C still fwApiVer 4
    check("refresh 2: C addressed by SN, VERSION_QUERY skipped for it (identity kept)",
          strcmp(s_log, want_refresh2) == 0);
    if (strcmp(s_log, want_refresh2) != 0) {
        printf("  got  %s\n", s_log);
    }
    {
        tfbus_device_t d[8];
        int nd = tfbus_master_devices(d, 8);
        const tfbus_device_t *c = NULL;
        for (int i = 0; i < nd; i++) {
            c = d[i].sn == 0x22222222 ? &d[i] : c;
        }
        check("refresh 2: C keeps hwType 3 / fwType 3 / fwApiVer 4 from the previous scan",
              c && c->hw_type == 3 && c->fw_type == 3 && c->fw_api_ver == 4 && c->known && c->addr == 3);
    }

    s_nsim = 2;
    s_sim[0].ver_len = 0;
    s_sim[1].discover = false;

    // ---- polling / online tracking
    tfbus_module_t *ma = tfbus_master_get(0), *mb = tfbus_master_get(1), *mc = tfbus_master_get(2);
    drv_reset();
    poll_module(ma);
    poll_module(ma);
    check_drv("first reply -> online once, on_status for every reply",
              "online:1 status:1:5:01 status:1:5:01 ");
    check("status cached", ma->online && ma->polls == 2 && ma->status_len == 5 && ma->status[4] == 0x05);

    drv_reset();
    poll_module(mb);
    poll_module(mb);
    poll_module(mb);
    check("absent driverless module: fails counted, stays offline", !mb->online && mb->fails == 3);

    s_sim[0].present = false;
    drv_reset();
    poll_module(ma);
    poll_module(ma);
    check("2 missed polls: still online", ma->online && s_drv[0] == 0);
    poll_module(ma);
    check_drv("3rd missed poll -> offline", "offline:1 ");
    s_sim[0].present = true;
    drv_reset();
    poll_module(ma);
    check_drv("back online on next reply", "online:1 status:1:5:01 ");
    check("fails reset", ma->fails == 0);

    s_sim[1].corrupt = true;
    drv_reset();
    poll_module(mc);
    poll_module(mc);
    poll_module(mc);
    check("bad-XOR replies count as missed polls", !mc->online && mc->fails == 3 && s_drv[0] == 0);
    s_sim[1].corrupt = false;

    // ---- rejoin: module plugged in after discovery / power-cycled (lost its address)
    s_sim[2] = (sim_mod_t){.sn = 0x11111111, .present = false, .status = {1, 0x00, 1, 1, 0x09}, .status_len = 5};
    s_nsim = 3;
    mb->drv = &s_fake; // driverless in the registry; give it the fake driver to see init
    drv_reset();
    rejoin(mb);
    check("rejoin of an absent module: ADDR_ASSIGN unanswered, no poll, no init",
          !mb->assigned && !mb->online && s_drv[0] == 0);
    s_sim[2].present = true;
    s_sim[2].ver_len = 3;
    memcpy(s_sim[2].ver, (uint8_t[]){2, 1, 7}, 3);
    log_reset();
    s_scans = 0;
    tfbus_master_on_scan(count_scan);
    tfbus_device_t dv[8];
    int nd0 = tfbus_master_devices(dv, 8);
    rejoin(mb);
    check("rejoin: ADDR_ASSIGN then STATUS_POLL", strncmp(s_log, "aaff02111111110255 aa02", 23) == 0);
    check_drv("rejoin: online, status, driver init re-run", "online:2 status:2:5:01 init:2 ");
    check("rejoin: assigned + addressed", mb->assigned && mb->online && s_sim[2].addr == 2);
    {
        int nd = tfbus_master_devices(dv, 8);
        const tfbus_device_t *d = nd == nd0 + 1 ? &dv[nd - 1] : NULL;
        check("rejoin of a module absent at discovery: added to tfbusDevices, CONFIG.JSON rewritten",
              d && d->sn == 0x11111111 && d->addr == 2 && d->known && d->hw_type == 0x99 &&
                  d->have_ver && d->ver[2] == 7 && s_scans == 1);
        s_sim[2].addr = 0; // power-cycled: lost its address, already listed
        mb->online = false;
        rejoin(mb);
        check("rejoin of a listed module: no duplicate, no rewrite",
              tfbus_master_devices(dv, 8) == nd && s_scans == 1 && mb->online);
    }
    tfbus_master_on_scan(NULL);
    mb->drv = NULL;
    s_nsim = 2;

    // ---- command queue
    uint8_t f[TFBUS_MAX_FRAME + 4];
    check("queue rejects frame > 32 bytes", tfbus_master_queue(f, TFBUS_MAX_FRAME + 1) == ESP_ERR_INVALID_ARG);
    size_t n = tfbus_status_poll(f, sizeof f, 9); // unicast to nobody
    tfbus_master_queue(f, n);
    n = tfbus_reset(f, sizeof f, TFBUS_BROADCAST);
    tfbus_master_queue(f, n);
    uint8_t lvl = 0x04;
    tfbus_tlv_t rec = {.type = 0x08, .meta = 1, .len = 1, .data = &lvl};
    n = tfbus_module_cmd(f, sizeof f, 1, &rec, 1);
    tfbus_master_queue(f, n);
    log_reset();
    drain_cmds();
    check("unicast without ACK retried 3x, broadcast sent once, ACKed cmd once",
          count_in_log("aa0904a7 ") == 3 && count_in_log("aaff0752 ") == 1 && count_in_log("aa01f0") == 1);
    check("queue drained", s_cmds->n == 0);
    for (int i = 0; i < 128; i++) {
        tfbus_master_queue(f, n);
    }
    check("queue full -> ESP_ERR_TIMEOUT", tfbus_master_queue(f, n) == ESP_ERR_TIMEOUT);
    drain_cmds();

    // Bus task (driver init): a full queue is flushed to the bus instead of dropping frames.
    TaskHandle_t saved = s_task;
    s_task = NULL; // == xTaskGetCurrentTaskHandle() stub
    int ok = 0;
    for (int i = 0; i < 300; i++) {
        log_reset(); // the log buffer cannot hold 300 frames
        ok += tfbus_master_queue(f, n) == ESP_OK;
    }
    check("bus task: 300 frames accepted, full queue flushed twice (300 - 2*128 left)",
          ok == 300 && s_cmds->n == 300 - 2 * 128);
    drain_cmds();
    s_task = saved;

    // ---- registry reset (Lua reload): driver deinit, contexts freed, queue dropped, bus parked
    check("bus parked until tfbus_master_start()", s_park);
    tfbus_master_start();
    check("start unparks the bus", !s_park);
    s_parked = false;
    check("reset times out while the bus task is still running", !tfbus_master_reset(30) && tfbus_master_count() == 3);
    s_parked = true; // the stubbed bus task never runs: pretend it parked
    tfbus_master_get(0)->drv_ctx = strdup("A");
    tfbus_master_get(1)->drv_ctx = strdup("B"); // driverless: freed without deinit
    tfbus_master_queue(f, n);
    drv_reset();
    check("reset: registry cleared", tfbus_master_reset(30) && tfbus_master_count() == 0 && s_park);
    check_drv("reset: deinit only for modules with a driver and a context", "deinit:1:A ");
    check("reset: queued frames dropped", s_cmds->n == 0);
    int A2 = lua_module(0x22222222, 0x1e, "mm_C");
    lua_getfield(L, A2, "_addr");
    check("reload: modules addressed from 1 again", tfbus_master_count() == 1 && lua_tointeger(L, -1) == 1 &&
                                                       tfbus_master_get(0)->drv_ctx == NULL);
    lua_settop(L, 0);

    // ---- bus task: parked until tfbus_master_start()
    s_parked = false;
    run_bus(10);
    check("bus task: parks (waits for a notification) until started", s_bus_blocked && s_parked);

    // ---- start: discovery, scan callback + completion, first round, driver init, polling rounds,
    // deferred driver calls run between polls
    s_deferred[0] = 0;
    check("defer: call queued", tfbus_master_defer(defer_fn, tfbus_master_get(0), "t1", 7));
    s_nsim = 2;
    s_sim[0].present = false;
    s_sim[1] = (sim_mod_t){.sn = 0x22222222, .present = true, .status = {1, 0x00, 1, 1, 0x07}, .status_len = 5};
    s_scans = 0;
    tfbus_master_on_scan(count_scan);
    drv_reset();
    log_reset();
    s_while_blocked = tfbus_master_start; // the Lua task starts the bus while it is parked
    run_bus(3);
    s_bus_on_notify = true;
    check("start: bus task unparked, discovery (RESET, SEEK), scan callback, completion signalled",
          !s_parked && strncmp(s_log, "aaff0752 aaff2a7f ", 18) == 0 && s_scans == 1 && host_binary_given);
    check_drv("start: first round before driver init, then one poll per round",
              "online:1 status:1:5:01 init:1 status:1:5:01 status:1:5:01 status:1:5:01 ");
    check("start: deferred call run by the bus task", strcmp(s_deferred, "mm_C:t1:7 ") == 0);

    // ---- module gone: offline after 3 missed polls, then only rejoin attempts every 20 rounds
    // (rounds 20..500 = 25 x 3 ADDR_ASSIGN tries); the "x/y online" summary runs at round 500
    s_sim[1].present = false;
    drv_reset();
    log_reset();
    s_bus_rounds = 501;
    tfbus_master_start();
    check("absent module: 3 missed polls, then a rejoin attempt every 20 rounds",
          s_tx_count[TFBUS_T_STATUS_POLL] == 3 && s_tx_count[TFBUS_T_ADDR_ASSIGN] == 3 + 25 * 3 &&
              !tfbus_master_get(0)->online && tfbus_master_get(0)->fails == 3);
    check_drv("absent module: driver init still runs, no online event", "init:1 ");

    // ---- req_refresh_modules: the bus task rescans and signals completion
    s_sim[1].present = true;
    s_sim[1].addr = 0;
    log_reset();
    s_bus_rounds = 1;
    check("refresh: completed by the bus task", tfbus_master_refresh(1000));
    check("refresh: RESET, then ADDR_ASSIGN first (no SEEK)", strncmp(s_log, "aaff0752 aaff022222222201", 25) == 0);
    s_bus_on_notify = false;
    xSemaphoreGive(s_scan_done); // completion left over from an earlier scan
    check("refresh: bus task not answering -> timeout (stale completion ignored)", !tfbus_master_refresh(10));
    {
        TaskHandle_t saved_task = s_task;
        s_task = NULL;
        check("refresh before tfbus_master_init() -> false", !tfbus_master_refresh(10));
        s_task = saved_task;
    }

    // ---- deferred driver calls: bounded queue, try-lock against the Lua task
    s_deferred[0] = 0;
    int queued = 0;
    for (int i = 0; i < 33; i++) {
        queued += tfbus_master_defer(defer_fn, tfbus_master_get(0), "q", (uint32_t)i);
    }
    check("defer: queue holds 32 calls, the 33rd is dropped", queued == 32);
    host_mutex_busy = 1;
    run_deferred();
    check("defer: calls wait while a Lua hook holds the driver lock", !s_deferred[0] && s_defer->n == 32);
    host_mutex_busy = 0;
    run_deferred();
    check("defer: calls run in order once the lock is free",
          s_defer->n == 0 && strncmp(s_deferred, "mm_C:q:0 mm_C:q:1 ", 18) == 0 && strstr(s_deferred, "mm_C:q:31 "));

    // ---- no modules in OM.LUA: one discovery, then the bus idles
    s_parked = true;
    check("reset before the empty-registry run", tfbus_master_reset(30) && tfbus_master_count() == 0);
    log_reset();
    s_bus_on_notify = true;
    tfbus_master_start();
    s_bus_on_notify = false;
    // the refresh that timed out above is still pending: this scan is a refresh (no SEEK)
    check("no modules: pending refresh served (RESET, DISCOVER), then the bus task waits",
          s_bus_blocked && strcmp(s_log, "aaff0752 aaff085d ") == 0);

    // ---- DISCOVER answered by several modules at once (bad XOR): 3 retries, then give up
    s_nsim = 1;
    s_sim[0] = (sim_mod_t){.sn = 0x0f05f124, .present = true, .discover = true, .garble = true, .ident = {3, 3, 4}};
    log_reset();
    discover(false);
    check("DISCOVER: garbled replies retried 3x, then given up", s_tx_count[TFBUS_T_DISCOVER] == 4 && !s_sim[0].addr);

    // ---- registry edge cases
    lua_newtable(L);
    lua_newtable(L);
    lua_setfield(L, -2, "_f");
    tfbus_obj_store(L, 1, 3, 1.5);
    lua_getfield(L, 1, "_f");
    lua_rawgeti(L, -1, 3);
    check("obj_store: non-integer value stored as a float", !lua_isinteger(L, -1) && lua_tonumber(L, -1) == 1.5);
    lua_settop(L, 0);

    lua_module(0x33333333, 0x1e, "mm_X");
    lua_module(0x33333333, 0x99, "mm_X2");
    check("module redeclared with another hwType: entry kept with the old type, name follows",
          tfbus_master_count() == 1 && tfbus_master_get(0)->hw_type == 0x1e && !strcmp(tfbus_master_get(0)->name, "mm_X2"));
    lua_settop(L, 0);
    for (int i = 1; i < TFBUS_MAX_MODULES; i++) {
        lua_module(0x40000000 + i, 0x99, "mm");
        lua_settop(L, 0);
    }
    int over = lua_module(0x4fffffff, 0x99, "mm_over");
    lua_getfield(L, over, "_addr");
    check("49th module: rejected (no address)", tfbus_master_count() == TFBUS_MAX_MODULES && lua_isnil(L, -1));
    lua_settop(L, 0);

    // all 48 addresses taken: a new module found by DISCOVER is listed but not addressed
    s_sim[0].garble = false;
    log_reset();
    discover(false);
    {
        tfbus_device_t d[TFBUS_MAX_MODULES];
        int nd = tfbus_master_devices(d, TFBUS_MAX_MODULES);
        check("DISCOVER with no free address: module listed (unknown, no address), left unaddressed",
              nd >= 1 && d[0].sn == 0x0f05f124 && !d[0].known && !d[0].addr && !s_sim[0].addr);
    }

    // driverless module: status changes are only logged
    {
        tfbus_module_t *m = tfbus_master_get(1);
        s_sim[0] = (sim_mod_t){.sn = m->sn, .present = true, .addr = m->addr, .status = {2, 0xaa}, .status_len = 2};
        poll_module(m);
        check("driverless module: status cached", m->online && m->status_len == 2 && m->status[1] == 0xaa);
    }
    s_parked = true;
    check("final reset", tfbus_master_reset(30) && tfbus_master_count() == 0);

    lua_close(L);
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
