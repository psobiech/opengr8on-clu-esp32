// Host tests for TFBUS module drivers: the real driver sources are compiled against stubs of the
// bus queue / Lua event API, and their output is compared with frames captured from a real CLU
// (clu firmware/tfbus_capture_20261014_171505.log).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "esp_timer.h"
#include "fake_nvs.h"
#include "lauxlib.h"
#include "lualib.h"
#include "tfbus_drivers.h"
#include "tfbus_master.h"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- stubs: bus queue
static char s_frames[32768];
static size_t s_flen;

static bool s_queue_full; // bus queue full: tfbus_master_queue() fails
esp_err_t tfbus_master_queue(const uint8_t *f, size_t len)
{
    if (s_queue_full) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t i = 0; i < len; i++) {
        s_flen += sprintf(s_frames + s_flen, "%02x", f[i]);
    }
    s_frames[s_flen++] = ' ';
    s_frames[s_flen] = 0;
    return ESP_OK;
}

static int count_frames(const char *log, const char *prefix)
{
    int n = 0;
    for (const char *p = log; *p; p = strchr(p, ' ') + 1) {
        n += strncmp(p, prefix, strlen(prefix)) == 0;
    }
    return n;
}

static void frames_reset(void)
{
    s_flen = 0;
    s_frames[0] = 0;
}

static void check_frames(const char *name, const char *want)
{
    int ok = strcmp(s_frames, want) == 0;
    check(name, ok);
    if (!ok) {
        printf("  got  %s\n  want %s\n", s_frames, want);
    }
}

static const tfbus_driver_t *s_registered[16];
static int s_nregistered;
void tfbus_driver_register(const tfbus_driver_t *d) { s_registered[s_nregistered++] = d; }

void tfbus_obj_store(lua_State *L, int obj, int idx, lua_Number v)
{
    lua_getfield(L, obj, "_f");
    lua_pushnumber(L, v);
    lua_rawseti(L, -2, idx);
    lua_pop(L, 1);
}

// ---------------------------------------------------------------- stubs: Lua event API
static char s_posts[8192];
static size_t s_plen;

void clu_lua_post_event(const char *n, int ev)
{
    s_plen += sprintf(s_posts + s_plen, "%s!%d ", n, ev);
}

void clu_lua_post_number(const char *n, int idx, double v)
{
    s_plen += sprintf(s_posts + s_plen, "%s[%d]=%g ", n, idx, v);
}

void clu_lua_post_string(const char *n, int idx, const char *v)
{
    s_plen += sprintf(s_posts + s_plen, "%s[%d]=%s ", n, idx, v);
}

static void posts_reset(void)
{
    s_plen = 0;
    s_posts[0] = 0;
}

static void check_posts(const char *name, const char *want)
{
    int ok = strcmp(s_posts, want) == 0;
    check(name, ok);
    if (!ok) {
        printf("  got  %s\n  want %s\n", s_posts, want);
    }
}

// ---------------------------------------------------------------- stubs: esp_timer
static int64_t s_now_us;
int64_t esp_timer_get_time(void) { return s_now_us; }
// Timers are recorded and fired explicitly by the test (timers_advance).
struct esp_timer {
    esp_timer_create_args_t a;
    int64_t due_us; // 0 = stopped
    bool deleted;
};
static struct esp_timer s_timers[32];
static int s_ntimers;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h)
{
    if (s_ntimers >= 32) {
        return ESP_FAIL;
    }
    s_timers[s_ntimers].a = *a;
    *h = &s_timers[s_ntimers++];
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us)
{
    if (h) {
        h->due_us = s_now_us + (int64_t)us;
    }
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t h)
{
    if (h) {
        h->due_us = 0;
    }
    return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t h)
{
    if (!h || h->due_us) {
        return ESP_ERR_INVALID_STATE; // like ESP-IDF: a running timer cannot be deleted
    }
    h->deleted = true;
    return ESP_OK;
}
// tfbus_master_defer: queued like the bus task does, run by run_deferred() (timers_advance runs
// them right after firing, i.e. the bus task got to them before any further Lua call).
static struct {
    tfbus_defer_fn_t fn;
    tfbus_module_t *m;
    void *arg;
    uint32_t tag;
} s_defer[32];
static int s_ndefer;
bool tfbus_master_defer(tfbus_defer_fn_t fn, tfbus_module_t *m, void *arg, uint32_t tag)
{
    if (s_ndefer >= 32) {
        return false;
    }
    s_defer[s_ndefer].fn = fn;
    s_defer[s_ndefer].m = m;
    s_defer[s_ndefer].arg = arg;
    s_defer[s_ndefer].tag = tag;
    s_ndefer++;
    return true;
}
static void run_deferred(void)
{
    for (int i = 0; i < s_ndefer; i++) {
        s_defer[i].fn(s_defer[i].m, s_defer[i].arg, s_defer[i].tag);
    }
    s_ndefer = 0;
}
// Fire due timers only (esp_timer task); their work waits in the defer queue.
static void timers_fire(int64_t us)
{
    s_now_us += us;
    for (int i = 0; i < s_ntimers; i++) {
        if (s_timers[i].deleted) {
            continue;
        }
        if (s_timers[i].due_us && s_timers[i].due_us <= s_now_us) {
            s_timers[i].due_us = 0;
            s_timers[i].a.callback(s_timers[i].a.arg);
        }
    }
}
static void timers_advance(int64_t us)
{
    timers_fire(us);
    run_deferred();
}

// ---------------------------------------------------------------- helpers
static lua_State *L;

static tfbus_module_t *module(const tfbus_driver_t *drv, uint8_t addr)
{
    tfbus_module_t *m = calloc(1, sizeof *m);
    m->drv = drv;
    m->addr = addr;
    strcpy(m->name, "mod");
    return m;
}

// Lua object table on the stack top: {_f = {}}
static int new_obj(void)
{
    lua_newtable(L);
    lua_newtable(L);
    lua_setfield(L, -2, "_f");
    return lua_gettop(L);
}

static void lua_set(tfbus_module_t *m, int cls, int ch, int idx, lua_Number v)
{
    int obj = new_obj();
    lua_pushnumber(L, v);
    m->drv->io_set(m, L, obj, cls, ch, idx, lua_gettop(L));
    lua_settop(L, obj - 1);
}

static void lua_set_str(tfbus_module_t *m, int cls, int ch, int idx, const char *v)
{
    int obj = new_obj();
    lua_pushstring(L, v);
    m->drv->io_set(m, L, obj, cls, ch, idx, lua_gettop(L));
    lua_settop(L, obj - 1);
}

// s_frames ("aa<addr><type><payload><crc> ...") -> "type:payload ..." with trailing 00 bytes
// trimmed down to a 4-byte minimum (same normalisation as the capture-derived expectations).
static void frames_tokens(char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    for (const char *p = s_frames; *p;) {
        const char *e = strchr(p, ' ');
        size_t n = (size_t)(e - p); // hex chars of the whole frame
        size_t pl = (n - 8) / 2;    // payload bytes: minus aa, addr, type, crc
        const char *pay = p + 6;
        while (pl > 4 && pay[2 * pl - 2] == '0' && pay[2 * pl - 1] == '0') {
            pl--;
        }
        o += snprintf(out + o, cap - o, "%.2s:%.*s ", p + 4, (int)(2 * pl), pay);
        p = e + 1;
    }
}

static void lua_exec(tfbus_module_t *m, int cls, int ch, int idx, int nargs, lua_Number a0)
{
    int obj = new_obj();
    if (nargs) {
        lua_pushnumber(L, a0);
    }
    m->drv->io_execute(m, L, obj, cls, ch, idx, obj + 1, nargs);
    lua_settop(L, obj - 1);
}

// execute with several numeric args; returns io_execute's result. Object left at the stack top
// slot `obj` if keep (caller pops), else popped.
static int lua_execn(tfbus_module_t *m, int cls, int ch, int idx, int nargs, const double *a)
{
    int obj = new_obj();
    for (int i = 0; i < nargs; i++) {
        lua_pushnumber(L, a[i]);
    }
    int r = m->drv->io_execute(m, L, obj, cls, ch, idx, obj + 1, nargs);
    lua_settop(L, obj - 1);
    return r;
}

// execute(idx, str [, ramp]) in a protected call (the driver may raise a Lua error); false on error.
static int exec_str_p(lua_State *Ls)
{
    tfbus_module_t *m = lua_touserdata(Ls, 1);
    int idx = (int)lua_tointeger(Ls, 2);
    int nargs = lua_gettop(Ls) - 3;
    lua_newtable(Ls);
    lua_setfield(Ls, 3, "_f");
    m->drv->io_execute(m, Ls, 3, 9, 0, idx, 4, nargs);
    return 0;
}

static bool lua_exec_str(tfbus_module_t *m, int idx, const char *str, int ramp)
{
    lua_pushcfunction(L, exec_str_p);
    lua_pushlightuserdata(L, m);
    lua_pushinteger(L, idx);
    lua_newtable(L);
    lua_pushstring(L, str);
    if (ramp >= 0) {
        lua_pushinteger(L, ramp);
    }
    bool ok = lua_pcall(L, ramp >= 0 ? 5 : 4, 0, 0) == LUA_OK;
    lua_settop(L, 0);
    return ok;
}

static void status(tfbus_module_t *m, const char *hexpl)
{
    uint8_t pl[64];
    size_t n = 0;
    for (const char *p = hexpl; *p;) {
        if (*p == ' ') {
            p++;
            continue;
        }
        unsigned b;
        sscanf(p, "%2x", &b);
        pl[n++] = (uint8_t)b;
        p += 2;
    }
    m->drv->on_status(m, pl, n);
}

int main(void)
{
    L = luaL_newstate();

    // ---------------- IO_MODULE_DIN_8
    {
        tfbus_module_t *m = module(&tfbus_drv_io_module_din8, 2);
        m->drv->io_new(m, 4, 0, "DOU0");
        m->drv->io_new(m, 4, 1, "DOU1");
        m->drv->io_new(m, 3, 0, "DIN0");
        frames_reset();
        m->drv->init(m);
        check_frames("IO_MODULE_DIN_8 init == capture (addr 2)",
            "aa02f006010901010157 aa02f009010f0400000100005a aa02f006010901010254 aa02f009010f0400010100005b "
            "aa02f006010901010452 aa02f009010f04000201000058 aa02f00601090101085e aa02f009010f04000301000059 "
            "aa02f006010901011046 aa02f009010f0400040100005e aa02f006010901012076 aa02f009010f0400050100005f "
            "aa02f006010901014016 aa02f009010f0400060100005c aa02f0060109010180d6 aa02f009010f0400070100005d "
            "aa02f0080104030000000056 aa02f009010f0400000000005b aa02f0080104030000010057 aa02f009010f0400010000005a "
            "aa02f0080104030000020054 aa02f009010f04000200000059 aa02f0080104030000030055 aa02f009010f04000300000058 "
            "aa02f0080104030000040052 aa02f009010f0400040000005f aa02f0080104030000050053 aa02f009010f0400050000005e "
            "aa02f0080104030000060050 aa02f009010f0400060000005d aa02f0080104030000070051 aa02f009010f0400070000005c ");

        frames_reset();
        lua_exec(m, 4, 1, 0, 0, 0); // DOU1:execute(0) Switch -> on
        check_frames("DOUT Switch ch1 -> ACTIVATE[0x02]", "aa02f006010801010255 ");

        posts_reset();
        status(m, "03 00 01 01 00 01 01 01 00 02 02 10 6c 09"); // first status: silent publish
        check_posts("IO first status publishes values only", "DIN0[0]=0 DOU0[0]=0 DOU1[0]=0 ");
        posts_reset();
        s_now_us = 1000000;
        status(m, "03 00 01 01 01 01 01 01 02 02 02 10 6c 09"); // DIN0 pressed, DOU1 on
        s_now_us = 1200000;
        status(m, "03 00 01 01 00 01 01 01 02 02 02 10 6c 09"); // DIN0 released after 200 ms
        check_posts("DIN press/release -> click, DOUT on event",
                    "DIN0[0]=1 DIN0!0 DIN0!1 DOU1[0]=1 DOU1!0 DOU1!1 DIN0[0]=0 DIN0!0 DIN0!2 DIN0!6 ");

        // Timed switch: DOU2:execute(1, 1500) -> on now, off after 1.5 s
        frames_reset();
        lua_exec(m, 4, 2, 1, 1, 1500);
        timers_advance(1499000);
        check_frames("DOUT SwitchOn(1500) -> ACTIVATE only before expiry", "aa02f006010801010453 ");
        frames_reset();
        timers_advance(1000);
        check_frames("DOUT SwitchOn(1500) -> DEACTIVATE at expiry", "aa02f006010901010452 ");
        frames_reset();
        lua_exec(m, 4, 3, 1, 1, 1000);
        lua_set(m, 4, 3, 0, 1); // untimed set cancels the timer
        timers_advance(2000000);
        check_frames("DOUT untimed set cancels pending revert",
                     "aa02f00601080101085f aa02f00601080101085f ");

        // Timer fired (revert queued for the bus task), then Lua switches before the bus task
        // ran it: the stale revert must not override the newer command.
        frames_reset();
        lua_exec(m, 4, 4, 1, 1, 1000);  // SwitchOn(1000) ch4: on, revert to off
        timers_fire(1000000);           // revert of ch4 queued
        lua_exec(m, 4, 4, 1, 0, 0);     // SwitchOn ch4 (untimed) before the bus task runs it
        frames_reset();
        run_deferred();
        check_frames("DOUT stale deferred revert dropped after newer command", "");

        // Lua reload: deinit stops and deletes pending timers (the master frees the context)
        lua_exec(m, 4, 2, 1, 1, 1500);
        frames_reset();
        m->drv->deinit(m);
        timers_advance(2000000);
        check_frames("DOUT deinit: pending revert never fires", "");
    }

    // ---------------- RELAY_DIN_4
    {
        tfbus_module_t *m = module(&tfbus_drv_relay_din4, 0x0b);
        frames_reset();
        m->drv->init(m);
        check_frames("RELAY_DIN_4 init == capture (addr 0x0b)",
            "aa0bf00601090101015e aa0bf007010b020000025c aa0bf009010f04000001000053 "
            "aa0bf00601090101025d aa0bf007010b020001025d aa0bf009010f04000101000052 "
            "aa0bf00601090101045b aa0bf007010b020002025e aa0bf009010f04000201000051 "
            "aa0bf006010901010857 aa0bf007010b020003025f aa0bf009010f04000301000050 "
            "aa0bf007010b020003025f aa0bf007010b020002025e aa0bf007010b020001025d aa0bf007010b020000025c ");
    }

    // ---------------- ROLLER_SH_DIN_3
    {
        tfbus_module_t *m = module(&tfbus_drv_roller_sh_din3, 5);
        for (int c = 0; c < 3; c++) {
            char n[16];
            sprintf(n, "ROL%d", c);
            m->drv->io_new(m, 24, c, n);
            lua_set(m, 24, c, 9, 30000); // OnInit: MaxTime (before bus init -> no reconfigure)
        }
        frames_reset();
        m->drv->init(m);
        check_frames("ROLLER_SH_DIN_3 init == capture (addr 5)",
            "aa05f0080104030016010046 aa05f009011704000001b80bf6 aa05f009011704000201b80bf4 "
            "aa05f009011704000301b80bf5 aa05f009011704000101000044 aa05f009010f0400000100005d "
            "aa05f007011c0200000146 "
            "aa05f0080104030016020045 aa05f009011704000002b80bf5 aa05f009011704000202b80bf7 "
            "aa05f009011704000302b80bf6 aa05f009011704000102000047 aa05f009010f0400010100005c "
            "aa05f007011c0200000245 "
            "aa05f0080104030016030044 aa05f009011704000003b80bf4 aa05f009011704000203b80bf6 "
            "aa05f009011704000303b80bf7 aa05f009011704000103000046 aa05f009010f0400020100005f "
            "aa05f007011c0200000344 ");

        frames_reset();
        lua_set(m, 24, 2, 9, 30000); // after init: reconfigure ch3
        check_frames("ROLLER_SH_DIN_3 reconfigure ch3 == capture",
            "aa05f00c020b020005000b0200060052 aa05f010021704000003b80b17040001030000ff "
            "aa05f009011704000203b80bf6 aa05f009011704000303b80bf7 aa05f0080104030016030044 "
            "aa05f007011c0200000344 ");

        frames_reset();
        lua_exec(m, 24, 2, 10, 1, 41); // SetPosition ch3 41% (docs wire example, addr 7 there)
        check_frames("SetPosition ch3 41% -> SET_POSITION[0,3,41]", "aa05f0080116030000032969 ");

        posts_reset();
        status(m, "04 01 01 02 00 16 03 00 01 64 00 16 03 00 02 00 5a 16 03 00 03 22 5a");
        check_posts("roller first status publishes positions",
                    "ROL0[7]=100 ROL1[7]=0 ROL2[7]=34 ");
        posts_reset();
        status(m, "04 01 01 02 20 16 03 00 01 64 00 16 03 00 02 00 5a 16 03 00 03 21 5a");
        check_posts("roller moving down ch3 -> state/position events",
                    "ROL2[7]=33 ROL2!8 ROL2[0]=2 ROL2[3]=0 ROL2[4]=1 ROL2!0 ROL2!3 ROL2!2 ");
    }

    // ---------------- ROLLER_SH_DIN (single, old firmware)
    {
        tfbus_module_t *m = module(&tfbus_drv_roller_sh_din, 1);
        m->drv->io_new(m, 24, 0, "ROL");
        frames_reset();
        m->drv->init(m);
        check_frames("ROLLER_SH_DIN init == capture (addr 1)", "aa01f00c020b020001000b0200020056 ");
    }

    // ---------------- TOUCH_PANEL_FM_4
    {
        tfbus_module_t *m = module(&tfbus_drv_touch_panel_fm4, 0x0e);
        for (int b = 0; b < 4; b++) {
            char n[16];
            sprintf(n, "BUT%d", b);
            m->drv->io_new(m, 7, b, n);
        }
        m->drv->io_new(m, 20, 0, "TEMP");
        m->drv->io_new(m, 21, 0, "LIGHT");
        frames_reset();
        m->drv->init(m);
        check_frames("TOUCH_PANEL_FM_4 init == capture (addr 0x0e)",
            "aa0ef008010103000100005e aa0ef009010f04000000000057 aa0ef008010103000200005d "
            "aa0ef009010f04000100000056 aa0ef008010103000400005b aa0ef009010f04000200000055 "
            "aa0ef0080101030008000057 aa0ef009010f04000300000054 aa0ef00901000400070102005c "
            "aa0ef00901000400070a020057 aa0ef0090100040008010a005b aa0ef00901000400080a0a0050 "
            "aa0ef006010301010151 ");

        posts_reset();
        status(m, "02 06 01 01 00 02 02 10 6c 09");      // temp 24.12
        status(m, "02 06 01 01 00 08 02 10 0e 00");      // VAL 14 = btn 2, event 0 (OnChange)
        status(m, "02 06 01 01 00 08 02 10 0f 00");      // VAL 15 = btn 2, event 1 (OnSwitchOn)
        status(m, "02 06 01 01 00 02 02 10 76 09");      // temp 24.22 (+0.10 >= threshold)
        check_posts("touch panel temp + button events",
                    "TEMP[0]=24.12 BUT2!0 BUT2[0]=1 BUT2!1 TEMP[0]=24.22 TEMP!0 TEMP!1 ");
    }

    // ---------------- SMART_PANEL_FM_4
    {
        tfbus_module_t *m = module(&tfbus_drv_smart_panel_fm4, 0x12);
        m->drv->io_new(m, 30, 2, "PB2");
        static const char *icons[4] = {"up", "down", "left", "right"}; // PAN7540:set(4..7, ...)
        for (int i = 0; i < 4; i++) {
            lua_set_str(m, 29, 0, 4 + i, icons[i]);
        }
        frames_reset();
        m->drv->init(m);
        // Every TX frame to addr 0x12 in clu firmware/tfbus_capture_20261014_171505.log, as
        // "type:payload" with trailing zero padding trimmed. The 8 gesture-icon blocks are cleaned:
        // the real CLU reuses its buffer and leaks bytes of earlier names into the padding.
        static const char *want =
            "17:010000 30:000a0000 30:00000000 30:00010000 17:020000 30:010a0000 30:01000000 30:01010000 17:0"
            "40000 30:020a0000 30:02000000 30:02010000 17:080000 30:030a0000 30:03000000 30:03010000 17:10000"
            "0 30:040a0000 30:04000000 30:04010000 17:200000 30:050a0000 30:05000000 30:05010000 17:400000 30"
            ":060a0000 30:06000000 30:06010000 17:800000 30:070a0000 30:07000000 30:07010000 17:000100 30:080"
            "a0000 30:08000000 30:08010000 17:000200 30:090a0000 30:09000000 30:09010000 17:000400 30:0a0a000"
            "0 30:0a000000 30:0a010000 17:000800 30:0b0a0000 30:0b000000 30:0b010000 17:001000 30:0c0a0000 30"
            ":0c000000 30:0c010000 17:002000 30:0d0a0000 30:0d000000 30:0d010000 17:004000 30:0e0a0000 30:0e0"
            "00000 30:0e010000 17:008000 30:0f0a0000 30:0f000000 30:0f010000 30:00047570 30:0005646f776e 30:0"
            "0026c656674 30:00037269676874 31:01050000 31:02881300 31:0c010000 31:0d030000 31:0e020000 31:100"
            "10000 31:11000000 31:14010000 31:15010000 31:16010000 16:00010200 16:000a0200 16:01010a00 16:010"
            "a0a00 31:12010000 31:12010100 31:12010301 31:12010401 31:12010302 31:12010402 31:12010303 31:120"
            "10403 31:12010304 31:12010404 31:12020000 31:12020100 31:12020301 31:12020401 31:12020302 31:120"
            "20402 31:12020303 31:12020403 31:12020304 31:12020404 31:12030000 31:12030100 31:12030301 31:120"
            "30401 31:12030302 31:12030402 31:12030303 31:12030403 31:12030304 31:12030404 31:12040000 31:120"
            "40100 31:12040301 31:12040401 31:12040302 31:12040402 31:12040303 31:12040403 31:12040304 31:120"
            "40404 31:12040000 31:12040100 31:12040401 31:12040301 31:1a0c0200 31:12040601 31:12040402 31:120"
            "40302 31:1a0d0200 31:12040602 31:12040403 31:12040303 31:1a0e0200 31:12040603 31:12040404 31:120"
            "40304 31:1a0f0200 31:12040604 31:12030000 31:12030100 31:12030401 31:12030301 31:1a080200 31:120"
            "30601 31:12030402 31:12030302 31:1a090200 31:12030602 31:12030403 31:12030303 31:1a0a0200 31:120"
            "30603 31:12030404 31:12030304 31:1a0b0200 31:12030604 31:12020000 31:12020100 31:12020401 31:120"
            "20301 31:1a040200 31:12020601 31:12020402 31:12020302 31:1a050200 31:12020602 31:12020403 31:120"
            "20303 31:1a060200 31:12020603 31:12020404 31:12020304 31:1a070200 31:12020604 31:12010000 31:120"
            "10100 31:12010401 31:12010301 31:1a000200 31:12010601 31:12010402 31:12010302 31:1a010200 31:120"
            "10602 31:12010403 31:12010303 31:1a020200 31:12010603 31:12010404 31:12010304 31:1a030200 31:120"
            "10604 16:010a0a00 16:000a0200 31:01050000 31:02881300 30:00037269676874 30:0005646f776e 30:00026"
            "c656674 30:00047570 31:0d030000 31:0e020000 31:0c010000 31:10000000 31:11000000 31:14010000 31:1"
            "5010000 31:16010000 17:008000 30:0f0a0000 30:0f000000 30:0f010000 17:004000 30:0e0a0000 30:0e000"
            "000 30:0e010000 17:002000 30:0d0a0000 30:0d000000 30:0d010000 17:001000 30:0c0a0000 30:0c000000 "
            "30:0c010000 17:000800 30:0b0a0000 30:0b000000 30:0b010000 17:000400 30:0a0a0000 30:0a000000 30:0"
            "a010000 17:000200 30:090a0000 30:09000000 30:09010000 17:000100 30:080a0000 30:08000000 30:08010"
            "000 17:800000 30:070a0000 30:07000000 30:07010000 17:400000 30:060a0000 30:06000000 30:06010000 "
            "17:200000 30:050a0000 30:05000000 30:05010000 17:100000 30:040a0000 30:04000000 30:04010000 17:0"
            "80000 30:030a0000 30:03000000 30:03010000 17:040000 30:020a0000 30:02000000 30:02010000 17:02000"
            "0 30:010a0000 30:01000000 30:01010000 17:010000 18:000002 30:000a0000 30:00000000 30:00010000 ";
        char got[sizeof s_frames];
        frames_tokens(got, sizeof got);
        check("SMART_PANEL init == capture (275 frames, addr 0x12)", strcmp(got, want) == 0);
        if (strcmp(got, want) != 0) {
            size_t i = 0;
            while (got[i] && got[i] == want[i]) i++;
            printf("  first diff at %zu\n  got  %.80s\n  want %.80s\n", i, got + i, want + i);
        }

        frames_reset();
        lua_set(m, 29, 0, 11, 3000); // runtime ProximityTimeout -> TFBUS_CMD 0x02
        check("SMART_PANEL runtime set(11, 3000) -> 0x31 [02 b8 0b]",
              strncmp(s_frames, "aa123102b80b", 12) == 0 && strlen(s_frames) == 26 * 2 + 1);

        posts_reset();
        status(m, "03 06 01 01 00 07 02 10 d7 00 08 02 10 12 00"); // light + VAL 18 = btn 2 OnLongPress
        check_posts("SMART_PANEL button 3 long press", "PB2!4 ");
        frames_reset();
        lua_exec(m, 30, 2, 1, 0, 0); // LedSwitchOn button 3
        check_frames("SMART_PANEL LedSwitchOn btn3 -> LED_CMD[04,00,02]", "aa1218040002a6 ");

        // PANEL freedraw methods -> TFBUS_CMD sub-cmd
        static const struct { int idx; const char *pfx; } draw[] = {
            {2, "aa123104"}, {7, "aa123109"}, {29, "aa123119"}, {0, "aa123100"}, {1, "aa12310300000000"}};
        for (size_t i = 0; i < sizeof draw / sizeof draw[0]; i++) {
            frames_reset();
            lua_exec(m, 29, 0, draw[i].idx, 0, 0);
            char name[64];
            snprintf(name, sizeof name, "SMART_PANEL execute(%d) -> %s", draw[i].idx, draw[i].pfx);
            check(name, strncmp(s_frames, draw[i].pfx, strlen(draw[i].pfx)) == 0 && !strchr(s_frames, ' ')[1]);
        }

        // Button 3 (ch 2, VAL = 14 + ev): press ON+CHANGE, release OFF+CHANGE, then CLICK
        static const char *press_click[] = {"01 08 02 10 0f 00", "01 08 02 10 0e 00", "01 08 02 10 10 00",
                                            "01 08 02 10 0e 00", "01 08 02 10 14 00"};
        posts_reset();
        for (int i = 0; i < 5; i++) status(m, press_click[i]);
        check_posts("SMART_PANEL monostable click", "PB2[0]=1 PB2!1 PB2!0 PB2[0]=0 PB2!2 PB2!0 PB2!6 ");
        lua_set(m, 30, 2, 1, 1); // Mode = Bistable
        posts_reset();
        frames_reset();
        for (int i = 0; i < 5; i++) status(m, press_click[i]);
        check_posts("SMART_PANEL bistable 1st click: toggles on, release ignored",
                    "PB2[0]=1 PB2!1 PB2!0 PB2!6 ");
        check_frames("SMART_PANEL bistable on -> LED_CMD[04,00,02] (green)", "aa1218040002a6 ");
        posts_reset();
        frames_reset();
        for (int i = 0; i < 5; i++) status(m, press_click[i]);
        check_posts("SMART_PANEL bistable 2nd click: toggles off", "PB2[0]=0 PB2!2 PB2!0 PB2!6 ");
        check_frames("SMART_PANEL bistable off -> LED_CMD[04,00,04] (off)", "aa1218040004a0 ");
        lua_set(m, 30, 2, 1, 2); // Mode = Locked
        posts_reset();
        for (int i = 0; i < 5; i++) status(m, press_click[i]);
        check_posts("SMART_PANEL locked: no events", "");
        lua_set(m, 30, 2, 1, 0);
    }

    // ---------------- IO_MODULE_DIN_8: hold / short / long, HoldDelay, re-init, value edge cases
    {
        tfbus_module_t *m = module(&tfbus_drv_io_module_din8, 3);
        m->drv->io_new(m, 3, 0, "BTN");
        m->drv->io_new(m, 3, 1, "BTN1");
        m->drv->io_new(m, 4, 0, "OUT");
        frames_reset();
        m->drv->init(m);
        status(m, "03 00 01 01 00 01 01 01 00 02 02 10 6c 09");
        posts_reset();
        s_now_us = 10000000;
        status(m, "03 00 01 01 01 01 01 01 00 02 02 10 6c 09"); // press
        s_now_us += 400000;
        status(m, "03 00 01 01 01 01 01 01 00 02 02 10 6c 09"); // 400 ms: before HoldDelay
        s_now_us += 100000;
        status(m, "03 00 01 01 01 01 01 01 00 02 02 10 6c 09"); // 500 ms: first OnHold
        s_now_us += 50000;
        status(m, "03 00 01 01 01 01 01 01 00 02 02 10 6c 09"); // 550 ms: within HoldInterval
        s_now_us += 50000;
        status(m, "03 00 01 01 01 01 01 01 00 02 02 10 6c 09"); // 600 ms: second OnHold
        s_now_us += 400000;
        status(m, "03 00 01 01 00 01 01 01 00 02 02 10 6c 09"); // release at 1000 ms
        check_posts("DIN hold: OnHold at HoldDelay then every HoldInterval, release -> OnShortPress",
                    "BTN[0]=1 BTN!0 BTN!1 BTN!5 BTN!5 BTN[0]=0 BTN!0 BTN!2 BTN!3 ");

        posts_reset();
        status(m, "03 00 01 01 01 01 01 01 00 02 02 10 6c 09");
        s_now_us += 2500000;
        status(m, "03 00 01 01 00 01 01 01 00 02 02 10 6c 09");
        check("DIN 2.5 s press -> OnLongPress", strstr(s_posts, "BTN!4 ") != NULL &&
                                                    strstr(s_posts, "BTN!3 ") == NULL);

        lua_set(m, 3, 0, 2, 1000); // HoldDelay = 1000 ms
        posts_reset();
        status(m, "03 00 01 01 01 01 01 01 00 02 02 10 6c 09");
        s_now_us += 900000;
        status(m, "03 00 01 01 01 01 01 01 00 02 02 10 6c 09");
        check("DIN HoldDelay=1000: no OnHold at 900 ms", strstr(s_posts, "!5") == NULL);
        s_now_us += 100000;
        status(m, "03 00 01 01 01 01 01 01 00 02 02 10 6c 09");
        check("DIN HoldDelay=1000: OnHold at 1000 ms", strstr(s_posts, "BTN!5 ") != NULL);
        status(m, "03 00 01 01 00 01 01 01 00 02 02 10 6c 09");

        posts_reset();
        status(m, "03 00 01 01 02 01 01 01 00 02 02 10 6c 09"); // undeclared-bit / other channel
        check("DIN ch1 edge reported under its own name", strncmp(s_posts, "BTN1[0]=1 BTN1!0 BTN1!1", 23) == 0);
        status(m, "03 00 01 01 00 01 01 01 00 02 02 10 6c 09");

        frames_reset();
        lua_exec(m, 4, 0, 1, 1, 2000); // SwitchOn(2000) while off -> on, revert to off
        lua_exec(m, 4, 1, 1, 1, 0);    // (ch1 has no object; still drives the output)
        frames_reset();
        // re-addressing (bus restart) re-applies the requested outputs instead of all-off
        m->drv->init(m);
        check("re-init keeps requested outputs ON (ch0, ch1)",
              strncmp(s_frames, "aa03f006010801010157 aa03f009010f0400000100005b aa03f006010801010254 ", 69) == 0);
        timers_advance(2000000);

        // timed command that does not change the state starts no timer
        lua_set(m, 4, 0, 0, 1);
        frames_reset();
        {   // SwitchOn(500) while already on (object's stored Value = 1)
            int obj = new_obj();
            tfbus_obj_store(L, obj, 0, 1);
            lua_pushinteger(L, 500);
            m->drv->io_execute(m, L, obj, 4, 0, 1, obj + 1, 1);
            lua_settop(L, obj - 1);
        }
        timers_advance(1000000);
        check_frames("SwitchOn(t) while on: no revert", "aa03f006010801010157 ");

        frames_reset();
        lua_set(m, 4, 0, 0, 0); // numeric 0 is off (Lua truthy)
        check_frames("set(Value, 0) -> DEACTIVATE", "aa03f006010901010156 ");
    }

    // ---------------- RELAY_DIN_4: output status -> DOUT events
    {
        tfbus_module_t *m = module(&tfbus_drv_relay_din4, 4);
        m->drv->io_new(m, 4, 2, "REL2");
        m->drv->init(m);
        posts_reset();
        status(m, "01 01 01 01 00");
        status(m, "01 01 01 01 04");
        status(m, "01 01 01 01 00");
        check_posts("relay status: first silent, then on/off events",
                    "REL2[0]=0 REL2[0]=1 REL2!0 REL2!1 REL2[0]=0 REL2!0 REL2!2 ");
    }

    // ---------------- ROLLER_SH_DIN_3: commands, timed move, block, position clamp
    {
        tfbus_module_t *m = module(&tfbus_drv_roller_sh_din3, 6);
        for (int c = 0; c < 3; c++) {
            char n[16];
            sprintf(n, "R%d", c);
            m->drv->io_new(m, 24, c, n);
        }
        m->drv->init(m);
        status(m, "04 01 01 02 00 16 03 00 01 32 00 16 03 00 02 32 00 16 03 00 03 32 00");

        frames_reset();
        lua_exec(m, 24, 0, 0, 0, 0); // MoveUp ch1
        lua_exec(m, 24, 1, 1, 0, 0); // MoveDown ch2
        lua_exec(m, 24, 2, 3, 0, 0); // Stop ch3 (docs example: DEACTIVATE 0x30)
        check_frames("roller up ch1 / down ch2 / stop ch3 bitmasks",
                     "aa06f006010801010152 aa06f00601080101085b aa06f006010901013062 ");

        frames_reset();
        lua_exec(m, 24, 0, 0, 1, 5000); // MoveUp(5000)
        timers_advance(4999000);
        check_frames("roller MoveUp(5000): no stop before expiry", "aa06f006010801010152 ");
        frames_reset();
        timers_advance(1000);
        check_frames("roller MoveUp(5000): DEACTIVATE at expiry", "aa06f006010901010351 ");

        // stop fired but still queued for the bus task when a new move starts: dropped
        lua_exec(m, 24, 0, 0, 1, 1000); // MoveUp(1000)
        timers_fire(1000000);
        lua_exec(m, 24, 0, 6, 0, 0);    // HoldDown (untimed)
        frames_reset();
        run_deferred();
        check_frames("roller stale deferred stop dropped after newer move", "");

        // moving (status says up on the active channel) -> MoveUp again stops
        status(m, "04 01 01 02 10 16 03 00 01 32 00 16 03 00 02 32 00 16 03 00 03 32 00");
        frames_reset();
        lua_exec(m, 24, 0, 0, 0, 0);
        check_frames("roller MoveUp while moving -> stop", "aa06f006010901010351 ");
        status(m, "04 01 01 02 00 16 03 00 01 32 00 16 03 00 02 32 00 16 03 00 03 32 00");

        frames_reset();
        posts_reset();
        lua_exec(m, 24, 1, 13, 1, 1); // SetRollerBlocked(1) ch2
        lua_exec(m, 24, 1, 1, 0, 0);  // MoveDown ignored
        lua_exec(m, 24, 1, 10, 1, 50); // SetPosition ignored
        check_frames("roller blocked: stop, then moves ignored", "aa06f006010901010c5e ");
        check_posts("roller blocked -> State 3", "R1[0]=3 ");
        lua_exec(m, 24, 1, 13, 1, 0);
        frames_reset();
        lua_exec(m, 24, 1, 1, 0, 0);
        check_frames("roller unblocked: MoveDown works", "aa06f00601080101085b ");

        frames_reset();
        lua_exec(m, 24, 0, 10, 1, 150);
        check_frames("roller SetPosition clamps to 100", "aa06f0080116030000016425 ");

        int nt = s_ntimers;
        m->drv->io_new(m, 24, 0, "R0b"); // same channel declared again (e.g. via req:)
        check("roller channel redeclared: no new esp_timer", s_ntimers == nt);

        lua_exec(m, 24, 2, 0, 1, 3000); // MoveUp(3000) pending at reload
        frames_reset();
        m->drv->deinit(m);
        timers_advance(5000000);
        check_frames("roller deinit: pending stop never fires", "");
    }

    // ---------------- TOUCH_PANEL_FM_4: LED, buzzer, sensor thresholds / range
    {
        tfbus_module_t *m = module(&tfbus_drv_touch_panel_fm4, 0x0f);
        m->drv->io_new(m, 7, 1, "B1");
        m->drv->io_new(m, 20, 0, "T");
        m->drv->init(m);
        frames_reset();
        lua_exec(m, 7, 1, 1, 0, 0);
        lua_exec(m, 7, 1, 2, 0, 0);
        check_frames("touch panel LedSwitchOn/Off btn1", "aa0ff008010103000201005d aa0ff008010103000200005c ");
        frames_reset();
        lua_set(m, 29, 0, 13, 0);
        check_frames("touch panel buzzer off (PANEL:set(13, 0))", "aa0ff006010301010051 ");

        lua_set(m, 20, 0, 1, 0.5); // Threshold 0.5
        lua_set(m, 20, 0, 4, 30);  // MaxValue 30
        posts_reset();
        status(m, "02 06 01 01 00 02 02 10 d0 07"); // 20.00
        status(m, "02 06 01 01 00 02 02 10 fa 07"); // 20.42 (< threshold: value only)
        status(m, "02 06 01 01 00 02 02 10 04 08"); // 20.52 (>= 0.5 from 20.00)
        status(m, "02 06 01 01 00 02 02 10 bc 0b"); // 30.04 out of range
        status(m, "02 06 01 01 00 02 02 10 c6 0b"); // 30.14 still out: no second OnOutOfRange
        check_posts("touch panel temp threshold + out of range",
                    "T[0]=20 T[0]=20.42 T[0]=20.52 T!0 T!1 T[0]=30.04 T!0 T!1 T!3 T[0]=30.14 ");

        posts_reset();
        status(m, "02 06 01 01 00 08 02 10 15 00"); // VAL 21 = btn 3 (not declared)
        status(m, "02 06 01 01 00 08 02 10 09 00"); // VAL 9 = btn 1, event 2 (OnSwitchOff)
        check_posts("touch panel: undeclared button ignored, btn1 off", "B1[0]=0 B1!2 ");
    }

    // ---------------- Lua build limits (same defines as components/lua/CMakeLists.txt)
    {
        char src[1024] = "return ";
        for (int i = 0; i < 100; i++) strcat(src, "(");
        strcat(src, "1");
        for (int i = 0; i < 100; i++) strcat(src, ")");
        check("100 nested parens parse (within LUAI_MAXCCALLS=120)", luaL_loadstring(L, src) == LUA_OK);
        lua_settop(L, 0);
        strcpy(src, "return ");
        for (int i = 0; i < 130; i++) strcat(src, "(");
        strcat(src, "1");
        for (int i = 0; i < 130; i++) strcat(src, ")");
        int st = luaL_loadstring(L, src);
        // 5.3: "chunk has too many C levels"; 5.4+: "C stack overflow" (luaE_checkcstack)
        const char *msg = st != LUA_OK ? lua_tostring(L, -1) : "";
        check("130 nested parens -> Lua error (C levels limit)",
              st != LUA_OK && (strstr(msg, "C levels") || strstr(msg, "C stack overflow")));
        lua_settop(L, 0);
    }

    // ---------------- driver table + queue helpers
    tfbus_drivers_register_all();
    {
        bool distinct = s_nregistered == 8;
        for (int i = 0; i < s_nregistered; i++) {
            for (int j = 0; j < i; j++) {
                distinct = distinct && s_registered[i]->hw_type != s_registered[j]->hw_type;
            }
        }
        check("register_all: 8 drivers, one per hwType", distinct);
    }
    {
        tfbus_module_t *m = module(&tfbus_drv_io_module_din8, 9);
        uint8_t big[40] = {0};
        tfbus_tlv_t rec = {.type = 1, .meta = 0, .len = sizeof big, .data = big};
        frames_reset();
        drv_queue_record(m, 1, 0, big, sizeof big); // does not fit a frame
        drv_queue_records(m, &rec, 1);
        drv_queue_frame(m, 0x30, big, sizeof big);
        s_queue_full = true;
        drv_queue_record(m, 1, 0, big, 1);
        drv_queue_records(m, &(tfbus_tlv_t){.type = 1, .len = 1, .data = big}, 1);
        drv_queue_frame(m, 0x30, big, 1);
        s_queue_full = false;
        check_frames("queue helpers: oversized records / full queue dropped (logged)", "");
    }

    // ---------------- SMART_PANEL_FM_4: runtime features, sensors, freedraw, button LEDs
    {
        tfbus_module_t *m = module(&tfbus_drv_smart_panel_fm4, 0x13);
        m->drv->io_new(m, 30, 0, "PB0");
        m->drv->io_new(m, 20, 0, "TMP");
        m->drv->io_new(m, 21, 0, "LGT");
        m->drv->init(m);

        frames_reset();
        lua_set(m, 19, 0, 1, 5);      // PANEL_PAGE content: not decoded, not sent
        lua_set_str(m, 19, 0, 2, "");
        lua_set(m, 19, 0, 3, 7);      // warned once only
        check_frames("PANEL_PAGE set: nothing sent", "");
        lua_set_str(m, 29, 0, 4, "up");
        check("PANEL GestureIconUp after init -> DISPLAY_BLOCK [00 04 'up']",
              strncmp(s_frames, "aa133000047570", 14) == 0 && !strchr(s_frames, ' ')[1]);

        // freedraw / beep
        static const struct { int idx; const char *want; } ex[] = {
            {3, "aa13310a0102026869"},     // PrintText(1, 2, "hi", 2)
            {21, "aa13310fd0072c01050000"}, // SetBeep(2000, 300, 5)
        };
        for (int i = 0; i < 2; i++) {
            int obj = new_obj();
            if (ex[i].idx == 3) {
                lua_pushinteger(L, 1), lua_pushinteger(L, 2), lua_pushstring(L, "hi"), lua_pushinteger(L, 2);
            } else {
                lua_pushinteger(L, 2000), lua_pushinteger(L, 300), lua_pushinteger(L, 5);
            }
            frames_reset();
            int r = m->drv->io_execute(m, L, obj, 29, 0, ex[i].idx, obj + 1, lua_gettop(L) - obj);
            lua_settop(L, obj - 1);
            char name[64];
            snprintf(name, sizeof name, "SMART_PANEL execute(%d) -> %s", ex[i].idx, ex[i].want);
            check(name, r == 0 && strncmp(s_frames, ex[i].want, strlen(ex[i].want)) == 0);
        }
        frames_reset();
        lua_exec(m, 29, 0, 3, 0, 0);  // PrintText(): x 0, y 0, font 1, no text
        lua_exec(m, 29, 0, 21, 0, 0); // SetBeep(): 1000 Hz, 100 ms, volume 8
        lua_exec(m, 29, 0, 99, 0, 0); // unknown: logged only
        check("SMART_PANEL PrintText() / SetBeep() defaults, unknown method sends nothing",
              strncmp(s_frames, "aa13310a000001000000", 20) == 0 &&
                  strstr(s_frames, " aa13310fe803640008000000") && strlen(s_frames) == 2 * (2 * 26 + 1)); // TFBUS_CMD frames are padded to 26 bytes

        // button LEDs; Mode change away from bistable turns a lit LED off
        frames_reset();
        lua_exec(m, 30, 0, 0, 0, 0);  // ShowOK
        lua_exec(m, 30, 0, 2, 0, 0);  // LedSwitchOff
        lua_exec(m, 30, 0, 9, 0, 0);  // not decoded
        check("SMART_PANEL button ShowOK / LedSwitchOff, unknown method sends nothing",
              strncmp(s_frames, "aa131801000", 11) == 0 && strstr(s_frames, " aa1318010004") &&
                  strlen(s_frames) == 2 * 2 * 7 + 2);
        {
            int obj = new_obj();
            check("SMART_PANEL execute on another class / channel -> -1",
                  m->drv->io_execute(m, L, obj, 5, 0, 0, obj + 1, 0) == -1 &&
                      m->drv->io_execute(m, L, obj, 30, 99, 0, obj + 1, 0) == -1);
            lua_settop(L, obj - 1);
        }
        lua_set(m, 30, 0, 1, 1); // Bistable
        posts_reset();
        status(m, "01 08 02 10 01 00");
        status(m, "01 08 02 10 00 00");
        check_posts("SMART_PANEL bistable press toggles PB0 on", "PB0[0]=1 PB0!1 PB0!0 ");
        frames_reset();
        lua_set(m, 30, 0, 1, 7); // invalid Mode -> monostable: lit LED switched off
        check("SMART_PANEL Mode bistable -> monostable while on: LED off", strncmp(s_frames, "aa1318010004", 12) == 0);

        // sensors: threshold / range from features, signed temperature, short records ignored
        lua_set(m, 20, 0, 1, 0.5);
        lua_set(m, 20, 0, 3, 10);
        lua_set(m, 20, 0, 4, 30);
        lua_set(m, 21, 0, 1, 2);
        posts_reset();
        status(m, "01 02 02 00 c4 09");             // 25.00
        status(m, "01 02 02 00 e2 09");             // 25.30: below the 0.5 threshold
        status(m, "01 02 02 00 1c 0c");             // 31.00: rise + out of range (max 30)
        status(m, "01 02 02 00 9c ff");             // -1.00: lower, still out of range
        status(m, "02 06 01 01 00 07 02 10 64 00"); // short record skipped; light 1.00
        check_posts("SMART_PANEL sensors: threshold, range, signed temperature, light",
                    "TMP[0]=25 TMP[0]=25.3 TMP[0]=31 TMP!0 TMP!1 TMP!3 TMP[0]=-1 TMP!0 TMP!2 LGT[0]=1 ");
    }

    // ---------------- ROLLER_SH_DIN_3: remaining commands and features
    {
        tfbus_module_t *m = module(&tfbus_drv_roller_sh_din3, 6);
        for (int c = 0; c < 3; c++) {
            char n[16];
            sprintf(n, "Q%d", c);
            m->drv->io_new(m, 24, c, n);
        }
        m->drv->init(m);
        frames_reset();
        lua_set(m, 24, 0, 14, 20000); // UpMax
        lua_set(m, 24, 0, 15, 21000); // DownMax
        lua_set(m, 24, 0, 16, 500);   // Offset
        lua_set(m, 24, 0, 17, 1);     // other feature: stored generically only
        lua_set(m, 7, 0, 14, 1);      // other class
        lua_set(m, 24, 5, 14, 1);     // no such channel
        check("roller UpMax/DownMax/Offset after init -> 3 channel reconfigurations",
              strncmp(s_frames, "aa06f0", 6) == 0 && count_frames(s_frames, "aa06f00c020b") == 3);

        frames_reset();
        lua_exec(m, 24, 0, 2, 0, 0); // Start: reverse of the default last direction (down) -> up
        check_frames("roller Start -> up", "aa06f006010801010152 ");
        frames_reset();
        lua_exec(m, 24, 0, 4, 0, 0); // Hold: direction change -> down
        lua_exec(m, 24, 0, 5, 0, 0); // HoldUp
        lua_exec(m, 24, 0, 8, 1, 0); // Calibration(0): down
        check("roller Hold (reverse) / HoldUp / Calibration(down)",
              strncmp(s_frames, "aa06f0060108010102", 18) == 0 && strstr(s_frames, " aa06f006010801010152 aa06f0060108010102"));
        frames_reset();
        lua_exec(m, 24, 0, 99, 0, 0);
        {
            int obj = new_obj();
            check("roller unknown method: nothing sent; other class / channel -> -1",
                  !s_frames[0] && m->drv->io_execute(m, L, obj, 7, 0, 0, obj + 1, 0) == -1 &&
                      m->drv->io_execute(m, L, obj, 24, 3, 0, obj + 1, 0) == -1);
            lua_settop(L, obj - 1);
        }

        lua_exec(m, 24, 1, 13, 1, 1); // block ch2
        posts_reset();
        status(m, "02 01 01 02 00 16 03 00 07 32 00"); // position of an unknown channel
        check("roller status: blocked channel keeps its state, unknown channel ignored",
              !strstr(s_posts, "Q1[") && !strstr(s_posts, "[7]"));
        m->drv->deinit(m);
    }

    // ---------------- ROLLER_SH_DIN (old firmware): no timing configuration on the wire
    {
        tfbus_module_t *m = module(&tfbus_drv_roller_sh_din, 2);
        m->drv->io_new(m, 24, 0, "RS");
        m->drv->init(m);
        frames_reset();
        lua_set(m, 24, 0, 1, 20000); // MaxTime
        lua_set(m, 24, 0, 14, 20000);
        check_frames("ROLLER_SH_DIN MaxTime / UpMax after init: nothing sent", "");
    }

    // ---------------- esp_timer exhausted: drivers keep working without their timers
    {
        int nt = s_ntimers;
        s_ntimers = 32;
        tfbus_module_t *r = module(&tfbus_drv_roller_sh_din3, 8);
        r->drv->io_new(r, 24, 0, "RT");
        tfbus_module_t *d = module(&tfbus_drv_io_module_din8, 10);
        d->drv->io_new(d, 4, 0, "DT");
        r->drv->init(r);
        frames_reset();
        lua_exec(r, 24, 0, 0, 1, 1000); // timed MoveUp without a stop timer
        lua_exec(d, 4, 0, 1, 0, 0);
        check_frames("no esp_timer left: roller MoveUp / DOUT SwitchOn still sent",
                     "aa08f00601080101015c aa0af00601080101015e ");
        s_ntimers = nt;
    }

    // ---------------- IO_MODULE_DIN_8 / TOUCH_PANEL_FM_4: remaining features
    {
        tfbus_module_t *m = module(&tfbus_drv_io_module_din8, 11);
        m->drv->io_new(m, 3, 0, "DI");
        m->drv->io_new(m, 4, 0, "DO");
        lua_set(m, 3, 0, 3, 200); // HoldInterval
        int obj = new_obj();
        check("DIN_8: unknown DOUT method -> -1", m->drv->io_execute(m, L, obj, 4, 0, 9, obj + 1, 0) == -1);
        lua_settop(L, obj - 1);

        tfbus_module_t *t = module(&tfbus_drv_touch_panel_fm4, 0x10);
        t->drv->io_new(t, 7, 0, "TB");
        t->drv->io_new(t, 21, 0, "TL");
        lua_set(t, 21, 0, 1, 5);
        lua_set(t, 21, 0, 3, 1);
        lua_set(t, 21, 0, 4, 50);
        frames_reset();
        lua_exec(t, 7, 0, 9, 0, 0);
        check_frames("touch panel unknown button method: nothing sent", "");
        posts_reset();
        status(t, "01 07 02 10 64 00");
        status(t, "01 07 02 10 a0 0f"); // 40.00: +39 >= threshold 5
        status(t, "01 07 02 10 70 17"); // 60.00: out of range (max 50)
        check_posts("touch panel light: first value, threshold, range",
                    "TL[0]=1 TL[0]=40 TL!0 TL!1 TL[0]=60 TL!0 TL!1 TL!3 ");
    }

    // ---------------- DIMMER_MOSFET_DIN
    {
        tfbus_module_t *m = module(&tfbus_drv_dimmer_mosfet_din, 1);
        m->sn = 0x1314874b;
        m->drv->io_new(m, 5, 0, "DIM");
        frames_reset();
        m->drv->init(m);
        check_frames("dimmer init: nothing sent before Lua sets a value (as the real CLU)", "");
        posts_reset();
        status(m, "02 01 02 10 2c 01 00 01 01 00");
        check_posts("dimmer first status: module level taken silently", "DIM[0]=0.3 ");

        lua_set(m, 5, 0, 3, 0.32); // MaxValue (OM.LUA OnInit)
        posts_reset();
        frames_reset();
        lua_set(m, 5, 0, 0, 0.32);
        check_frames("dimmer SetValue(0.32) == capture (RampTime 500)",
                     "aa01f00a0106050000f4010000a6 aa01f008010c03000040011c ");
        check_posts("dimmer 0.3 -> 0.32: change + rise", "DIM!0 DIM!3 ");

        posts_reset();
        frames_reset();
        lua_set(m, 5, 0, 0, 0.9);
        check_posts("dimmer SetValue above MaxValue -> OnOutOfRange, clamped (no change)", "DIM!5 ");
        lua_set(m, 5, 0, 1, 0); // RampTime 0
        frames_reset();
        posts_reset();
        double sw[] = {0, 1000};
        lua_execn(m, 5, 0, 1, 2, sw); // Switch(0, 1000): on -> off
        check_frames("dimmer Switch(0, 1000) -> 0 with ramp 1000",
                     "aa01f00a0106050000e8030000b8 aa01f008010c03000000005d ");
        check_posts("dimmer off: change, lower, OnSwitchOff", "DIM!0 DIM!4 DIM!2 ");

        frames_reset();
        posts_reset();
        double on[] = {2000};
        lua_execn(m, 5, 0, 2, 1, on); // SwitchOn(2000): MaxValue for 2 s, ramp = RampTime 0
        check_frames("dimmer SwitchOn(2000) -> MaxValue, ramp RampTime",
                     "aa01f00a01060500000000000053 aa01f008010c03000040011c ");
        timers_advance(1999000);
        check_frames("dimmer timed: still on before expiry",
                     "aa01f00a01060500000000000053 aa01f008010c03000040011c ");
        frames_reset();
        timers_advance(1000);
        check_frames("dimmer timed: back to 0 at expiry", "aa01f00a01060500000000000053 aa01f008010c03000000005d ");
        check_posts("dimmer timed: on events, Value posted + off events at expiry",
                    "DIM!0 DIM!3 DIM!1 DIM[0]=0 DIM!0 DIM!4 DIM!2 ");

        lua_set(m, 5, 0, 0, 0.1);
        lua_set(m, 5, 0, 0, 0);
        frames_reset();
        double mode[] = {0, 0, 1};
        lua_execn(m, 5, 0, 2, 3, mode); // SwitchOn LastValue
        check_frames("dimmer SwitchOn Mode LastValue -> 0.1",
                     "aa01f00a01060500000000000053 aa01f008010c030000640039 ");

        // HoldValue: 3000 ms sweep; first call 100 ms step = 0.32/30 (max-min = 0.32)
        lua_set(m, 5, 0, 0, 0);
        s_now_us += 10000000;
        frames_reset();
        double hr[] = {3200};
        lua_execn(m, 5, 0, 0, 1, hr);
        s_now_us += 100000;
        lua_execn(m, 5, 0, 0, 1, hr);
        check_frames("dimmer HoldValue steps up (10/1000 per 100 ms at 3200 ms sweep)",
                     "aa01f00a01060500006400000037 aa01f008010c0300000a0057 "
                     "aa01f00a01060500006400000037 aa01f008010c030000140049 ");
        s_now_us += 2000000;
        frames_reset();
        lua_execn(m, 5, 0, 0, 1, hr); // new hold: reverses
        check_frames("dimmer HoldValue: new hold reverses", "aa01f00a01060500006400000037 aa01f008010c0300000a0057 ");

        // Status: a level other than the target (after the hold-off) is taken over
        s_now_us += 2000000;
        posts_reset();
        status(m, "02 01 02 10 0a 00 00 01 01 00");
        check_posts("dimmer status == target: nothing", "");
        status(m, "02 01 02 10 00 00 00 01 01 00");
        check_posts("dimmer status level dropped (module power cycle): Value + events",
                    "DIM[0]=0 DIM!0 DIM!4 DIM!2 ");
        lua_set(m, 5, 0, 0, 0.2);
        posts_reset();
        status(m, "02 01 02 10 00 00 00 01 01 00");
        check_posts("dimmer stale status right after a command ignored", "");
        frames_reset();
        m->drv->init(m);
        check_frames("dimmer init after re-addressing re-applies the level",
                     "aa01f00a01060500000000000053 aa01f008010c030000c80095 ");
        check("dimmer: other class / unknown method -> -1",
              lua_execn(m, 5, 0, 9, 0, NULL) == -1 && lua_execn(m, 4, 0, 1, 0, NULL) == -1);

        // fw 3.0.2: Switch* Ramp 0 = RampTime; SetValue keeps 0 = instant
        lua_set(m, 5, 0, 1, 700); // RampTime
        frames_reset();
        double r0[] = {0, 0};
        lua_execn(m, 5, 0, 3, 2, r0); // SwitchOff(0, 0)
        check_frames("dimmer SwitchOff(0, Ramp 0) -> RampTime 700 (fw 3.0.2)",
                     "aa01f00a0106050000bc020000ed aa01f008010c03000000005d ");

        // fw 3.0.2: last value in non-volatile memory, debounced
        nvs_reset();
        lua_set(m, 5, 0, 0, 0.25);
        timers_advance(4999000);
        check("dimmer last value: not written before 5 s", nvs_commits == 0);
        lua_set(m, 5, 0, 0, 0.27);
        timers_advance(4999000);
        check("dimmer last value: restarted by a newer change", nvs_commits == 0);
        timers_advance(1000);
        const nvs_entry_t *e = nvs_peek("clu_light", "d1314874b");
        uint32_t lvl = 0;
        if (e) {
            memcpy(&lvl, e->val, sizeof lvl);
        }
        check("dimmer last value: written 5 s after the last change (NVS clu_light/d1314874b = 270)",
              nvs_commits == 1 && lvl == 270);
        lua_set(m, 5, 0, 0, 0);
        timers_advance(5000000);
        check("dimmer last value: switching off does not overwrite it", nvs_commits == 1);
        m->drv->deinit(m);
        free(m->drv_ctx);
        m->drv_ctx = NULL;
        m->drv->io_new(m, 5, 0, "DIM"); // CLU restart: state rebuilt, last value from NVS
        frames_reset();
        double lv[] = {0, 0, 1};
        lua_execn(m, 5, 0, 2, 3, lv); // SwitchOn LastValue
        check_frames("dimmer last value survives a restart (SwitchOn LastValue -> 0.27)",
                     "aa01f00a0106050000f4010000a6 aa01f008010c0300000e0152 ");
        lua_set(m, 5, 0, 0, 0.5);
        m->drv->deinit(m);
        check("dimmer deinit flushes a pending last-value save", nvs_commits == 2);
    }

    // ---------------- LED_RGBW_DIN
    {
        tfbus_module_t *m = module(&tfbus_drv_led_rgbw_din, 5);
        m->drv->io_new(m, 9, 0, "LED");
        m->drv->io_new(m, 12, 0, "POW");
        frames_reset();
        m->drv->init(m);
        check_frames("RGBW init: nothing sent before Lua sets a colour", "");
        posts_reset();
        double red[] = {0x73, 42};
        lua_execn(m, 9, 0, 3, 2, red);
        check_frames("RGBW SetRedValue(115, 42) == capture",
                     "aa05f00a01060500002a0000007d aa05f0160405021000730502100100050210020005021003003e ");
        check_posts("RGBW first colour: HSV/RGB published via the object, change/rise/on",
                    "LED!0 LED!3 LED!1 ");

        frames_reset();
        posts_reset();
        lua_exec_str(m, 6, "#00FF80", 0);
        check_frames("RGBW SetRGBvalue(#00FF80, 0)",
                     "aa05f00a01060500000000000057 aa05f01604050210000005021001ff0502100280050210030032 ");
        check("RGBW SetRGBvalue malformed -> Lua error", !lua_exec_str(m, 6, "#12345", -1));

        double val[] = {0.5};
        frames_reset();
        lua_execn(m, 9, 0, 0, 1, val); // SetValue(0.5): hue 150, sat 1 -> 00 80 40
        check_frames("RGBW SetValue(0.5) keeps hue/saturation",
                     "aa05f00a01060500000000000057 aa05f0160405021000000502100180050210024005021003008d ");
        double hue[] = {0};
        frames_reset();
        lua_execn(m, 9, 0, 1, 1, hue);
        check_frames("RGBW SetHue(0) -> red at V 0.5",
                     "aa05f00a01060500000000000057 aa05f016040502100080050210010005021002000502100300cd ");
        lua_set(m, 9, 0, 10, 50); // RedCorrection 50 %: re-sent at once
        double white[] = {200};
        lua_execn(m, 9, 0, 12, 1, white);
        frames_reset();
        posts_reset();
        double off[] = {0};
        lua_execn(m, 9, 0, 10, 1, off); // SwitchOff
        check_frames("RGBW SwitchOff: all channels 0",
                     "aa05f00a01060500000000000057 aa05f0160405021000000502100100050210020005021003004d ");
        check_posts("RGBW off: change, lower, OnSwitchOff", "LED!0 LED!4 LED!2 ");
        frames_reset();
        double sw[] = {0, 300};
        lua_execn(m, 9, 0, 11, 2, sw); // Switch: on -> MaxValue, red (corrected 50 %), W back
        check_frames("RGBW Switch on: MaxValue, R corrected 50 %, W restored",
                     "aa05f00a01060500002c0100007a aa05f0160405021000800502100100050210020005021003c805 ");

        // PowerSupplyVoltage from status (0.01 V)
        posts_reset();
        status(m, "01 02 02 10 72 09"); // 24.18 (CLU_2): above default MaxValue 24 -> OnOutOfRange
        status(m, "01 02 02 10 5e 09"); // 23.98: back in range; < 0.5 V, no change event
        status(m, "01 02 02 10 34 08"); // 21.00: still in range, change -2.98
        status(m, "01 02 02 10 d0 07"); // 20.00 out of range
        status(m, "01 02 02 10 60 09"); // 24.00 back
        check_posts("PowerSupplyVoltage: first value, range, sensitivity",
                    "POW[0]=24.18 POW[1]=100.75 POW!3 POW[0]=23.98 POW[1]=99.92 POW!6 "
                    "POW[0]=21 POW[1]=87.5 POW!0 POW!1 "
                    "POW[0]=20 POW[1]=83.33 POW!0 POW!1 POW!3 POW[0]=24 POW[1]=100 POW!0 POW!2 POW!6 ");

        // Timed switch restores the previous colour (bus task -> posts incl. RGB string)
        frames_reset();
        posts_reset();
        double t[] = {500};
        lua_execn(m, 9, 0, 10, 1, t); // SwitchOff for 0.5 s
        timers_advance(500000);
        check_posts("RGBW SwitchOff(500): restored, features + RGB string posted",
                    "LED!0 LED!4 LED!2 LED[0]=1 LED[1]=0 LED[2]=1 LED[3]=255 LED[4]=0 LED[5]=0 "
                    "LED[15]=200 LED[6]=#FF0000 LED!0 LED!3 LED!1 ");
        m->drv->deinit(m);
    }

    lua_close(L);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
