/**
 * @file drv_roller.c
 * @brief Roller shutter modules (shared implementation).
 *
 * - ROLLER_SH_DIN_3 (hwType 42 / 0x2a): 3x ROLLER_SHUTTER (class 24), PowerSupplyVoltage (12).
 * - ROLLER_SH_DIN (hwType 23 / 0x17, fwApiVer 1): 1x ROLLER_SHUTTER. Real CLU init sends only
 *   CHANNEL_MAP{[1,0],[2,0]}; OnInit uses old indices (1 = MaxTime, 5 = Overcurrent, 6 = VoltageType).
 *
 * ROLLER_SH_DIN_3 interface: module_2_0_ROLLER_SHUTTER_DIN_3_fv03_02.xml. Lua channel is 0-based,
 * wire channel 1-based.
 *
 * Verified against a real CLU startup capture (per channel n = 1..3):
 * @verbatim
 *   CHANNEL_INIT[0x16,n,0], FEATURE_PARAM[0,n,max], [2,n,up], [3,n,down], [1,n,offset],
 *   MODULE_PARAM[n-1,1,0,0], ROLLER_CHAN_REG[0,n]       (times in 10 ms, LE u16; up/down 0 -> max)
 * @endverbatim
 * Reconfiguration (after OnInit sets timing, observed ~3 s after init, channels descending):
 * @verbatim
 *   CHANNEL_MAP{[2n-1,0],[2n,0]}, FEATURE_PARAM{[0,n,max],[1,n,offset]}, [2,n,up], [3,n,down],
 *   CHANNEL_INIT[0x16,n,0], ROLLER_CHAN_REG[0,n]
 * @endverbatim
 * Documented (docs/TFBUS_ROLLER_SH_DIN_3.md): SET_POSITION[0x16] = [flags=0, n, pos%].
 *
 * UNVERIFIED (inferred from single-channel docs + DIN_3 stop example 0x30 = channel 3):
 * @verbatim
 *   up = ACTIVATE[1<<2c], down = ACTIVATE[1<<(2c+1)], stop = DEACTIVATE[3<<2c]   (c = n-1)
 * @endverbatim
 * Status:
 * @verbatim
 *   04 | 01 01 xx [state] | 3x 16 03 00 [n, pos%, flags]   (state 1/0x10 up, 2/0x20 down)
 * @endverbatim
 */
#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include "tfbus_drivers.h"
#include "tfbus_master.h"

static const char *TAG = "drv_rol3";

#define CH 3 // max channels
#define CLS_ROLLER 24

/** @brief Per-variant parameters. */
typedef struct {
    int nch;
    bool full_init;   // CHANNEL_INIT/FEATURE_PARAM/... (DIN_3) vs CHANNEL_MAP only (old single)
    int maxtime_idx;  // Lua feature index of MaxTime
} roller_cfg_t;

static const roller_cfg_t CFG_DIN3 = {.nch = 3, .full_init = true, .maxtime_idx = 9};
static const roller_cfg_t CFG_DIN1 = {.nch = 1, .full_init = false, .maxtime_idx = 1};

/** @brief ROLLER_SHUTTER feature indices. */
enum { F_STATE = 0, F_UP = 3, F_DOWN = 4, F_POSITION = 7, F_MAXTIME = 9, F_UPMAX = 14, F_DOWNMAX = 15,
       F_OFFSET = 16 };
/** @brief ROLLER_SHUTTER event indices. */
enum { EV_STATE = 0, EV_UP = 1, EV_DOWN = 2, EV_START = 3, EV_STOP = 4, EV_POSITION = 8 };
/** @brief Logical state (feature 0). */
enum { ST_STOP = 0, ST_UP = 1, ST_DOWN = 2, ST_BLOCKED = 3, ST_CALIB = 4 };

typedef struct roller roller_t;

/** @brief Per-channel stop timer (timed moves). */
typedef struct {
    roller_t *r;
    int ch;
    volatile uint32_t gen; // bumped on every move/stop: a fire queued before is stale
    int64_t due_us;        // stop deadline [esp_timer us], 0 = none (driver lock)
    esp_timer_handle_t timer;
} chan_timer_t;

/** @brief Driver state per module (drv_ctx). */
struct roller {
    tfbus_module_t *m;
    const roller_cfg_t *cfg;
    char name[CH][24];
    uint32_t max_ms[CH], up_ms[CH], down_ms[CH], offset_ms[CH];
    bool blocked[CH];
    int state[CH];       // logical, as reported to Lua
    int pos[CH];         // -1 unknown
    int last_dir[CH];    // ST_UP / ST_DOWN, for Start()
    int active_ch;       // channel the module-wide state byte refers to
    bool initialized;
    chan_timer_t stop_timer[CH];
};

/** @brief Driver state of @p m, allocated on first use. */
static roller_t *ctx(tfbus_module_t *m)
{
    if (!m->drv_ctx) {
        roller_t *r = calloc(1, sizeof *r);
        if (!r) {
            abort(); // out of memory while loading OM.LUA: reboot instead of a NULL dereference
        }
        r->m = m;
        r->cfg = m->drv->cfg;
        r->active_ch = -1;
        for (int i = 0; i < CH; i++) {
            r->max_ms[i] = 30000;
            r->pos[i] = -1;
            r->last_dir[i] = ST_DOWN;
        }
        m->drv_ctx = r;
    }
    return m->drv_ctx;
}

/**
 * @brief Build a FEATURE_PARAM record [idx, n, time LE u16 in 10 ms units] (saturates at 0xFFFF).
 */
static void param(uint8_t out[4], uint8_t idx, int n, uint32_t ms)
{
    uint32_t v = ms / 10;
    if (v > 0xFFFF) {
        v = 0xFFFF;
    }
    out[0] = idx;
    out[1] = (uint8_t)n;
    out[2] = (uint8_t)v;
    out[3] = (uint8_t)(v >> 8);
}

/** @brief Up travel time of channel @p c (MaxTime if unset). */
static uint32_t up_time(roller_t *r, int c)
{
    return r->up_ms[c] ? r->up_ms[c] : r->max_ms[c];
}

/** @brief Down travel time of channel @p c (MaxTime if unset). */
static uint32_t down_time(roller_t *r, int c)
{
    return r->down_ms[c] ? r->down_ms[c] : r->max_ms[c];
}

/** @brief Queue the startup init sequence of one channel (ROLLER_SH_DIN_3). */
static void init_channel(roller_t *r, int c)
{
    tfbus_module_t *m = r->m;
    int n = c + 1;
    uint8_t p[4];
    drv_queue_record(m, 0x04, 0x00, (uint8_t[]){0x16, (uint8_t)n, 0}, 3); // CHANNEL_INIT
    param(p, 0, n, r->max_ms[c]);
    drv_queue_record(m, 0x17, 0x00, p, 4);
    param(p, 2, n, up_time(r, c));
    drv_queue_record(m, 0x17, 0x00, p, 4);
    param(p, 3, n, down_time(r, c));
    drv_queue_record(m, 0x17, 0x00, p, 4);
    param(p, 1, n, r->offset_ms[c]);
    drv_queue_record(m, 0x17, 0x00, p, 4);
    drv_queue_record(m, 0x0f, 0x00, (uint8_t[]){(uint8_t)c, 1, 0, 0}, 4);   // MODULE_PARAM
    drv_queue_record(m, 0x1c, 0x00, (uint8_t[]){0, (uint8_t)n}, 2);         // ROLLER_CHAN_REG
}

/**
 * @brief Queue the reconfiguration sequence of one channel after its timing changed (ROLLER_SH_DIN_3).
 */
static void reconfigure_channel(roller_t *r, int c)
{
    tfbus_module_t *m = r->m;
    int n = c + 1;
    uint8_t a[2] = {(uint8_t)(2 * n - 1), 0}, b[2] = {(uint8_t)(2 * n), 0};
    tfbus_tlv_t map[2] = {{.type = 0x0b, .meta = 0, .len = 2, .data = a},
                          {.type = 0x0b, .meta = 0, .len = 2, .data = b}};
    drv_queue_records(m, map, 2); // CHANNEL_MAP up/down relay pair

    uint8_t p0[4], p1[4], p[4];
    param(p0, 0, n, r->max_ms[c]);
    param(p1, 1, n, r->offset_ms[c]);
    tfbus_tlv_t fp[2] = {{.type = 0x17, .meta = 0, .len = 4, .data = p0},
                         {.type = 0x17, .meta = 0, .len = 4, .data = p1}};
    drv_queue_records(m, fp, 2);
    param(p, 2, n, up_time(r, c));
    drv_queue_record(m, 0x17, 0x00, p, 4);
    param(p, 3, n, down_time(r, c));
    drv_queue_record(m, 0x17, 0x00, p, 4);
    drv_queue_record(m, 0x04, 0x00, (uint8_t[]){0x16, (uint8_t)n, 0}, 3);
    drv_queue_record(m, 0x1c, 0x00, (uint8_t[]){0, (uint8_t)n}, 2);
}

/**
 * @brief tfbus_driver_t::init: full per-channel init (DIN_3) or CHANNEL_MAP pairs only (single channel).
 */
static void init(tfbus_module_t *m)
{
    roller_t *r = ctx(m);
    for (int c = 0; c < r->cfg->nch; c++) {
        if (r->cfg->full_init) {
            init_channel(r, c);
        } else {
            uint8_t a[2] = {(uint8_t)(2 * c + 1), 0}, b[2] = {(uint8_t)(2 * c + 2), 0};
            tfbus_tlv_t map[2] = {{.type = 0x0b, .len = 2, .data = a}, {.type = 0x0b, .len = 2, .data = b}};
            drv_queue_records(m, map, 2);
        }
    }
    r->initialized = true;
}

/** @brief Stop channel @p c (DEACTIVATE both directions) and cancel its stop timer. */
static void cmd_stop(roller_t *r, int c)
{
    r->stop_timer[c].gen++;
    r->stop_timer[c].due_us = 0;
    esp_timer_stop(r->stop_timer[c].timer);
    drv_queue_record(r->m, 0x09, 0x01, (uint8_t[]){(uint8_t)(3 << (2 * c))}, 1);
}

/**
 * @brief Timed move expired: stop the channel unless the timer was restarted / cancelled (@p gen).
 *
 * Bus task (deferred), under the driver lock.
 */
static void stop_timer_fire(tfbus_module_t *m, void *arg, uint32_t gen)
{
    chan_timer_t *t = arg;
    if (gen == t->gen && t->due_us) {
        cmd_stop(t->r, t->ch);
    }
}

/** @brief tfbus_driver_t::overdue: stop channels whose timed move expired but whose deferred stop was dropped. */
static void overdue(tfbus_module_t *m)
{
    roller_t *r = m->drv_ctx;
    int64_t now = esp_timer_get_time();
    for (int c = 0; c < CH; c++) {
        if (r->stop_timer[c].due_us && now >= r->stop_timer[c].due_us) {
            cmd_stop(r, c);
        }
    }
}

/** @brief esp_timer task: hand the stop over to the bus task (no blocking queue send here). */
static void stop_timer_cb(void *arg)
{
    chan_timer_t *t = arg;
    tfbus_master_defer(stop_timer_fire, t->r->m, t, t->gen);
}

/** @brief tfbus_driver_t::deinit: stop and delete the stop timers. */
static void deinit(tfbus_module_t *m)
{
    roller_t *r = m->drv_ctx;
    for (int c = 0; c < CH; c++) {
        if (r->stop_timer[c].timer) {
            esp_timer_stop(r->stop_timer[c].timer);
            esp_timer_delete(r->stop_timer[c].timer);
            r->stop_timer[c].timer = NULL;
        }
    }
}

/**
 * @brief Move channel @p c in @p dir (ST_UP / ST_DOWN); stop after @p time_ms if > 0. Ignored while blocked.
 */
static void cmd_move(roller_t *r, int c, int dir, uint32_t time_ms)
{
    if (r->blocked[c]) {
        return;
    }
    r->active_ch = c;
    r->last_dir[c] = dir;
    uint8_t bit = (uint8_t)(1 << (2 * c + (dir == ST_DOWN ? 1 : 0)));
    drv_queue_record(r->m, 0x08, 0x01, &bit, 1);
    r->stop_timer[c].gen++;
    r->stop_timer[c].due_us = 0;
    esp_timer_stop(r->stop_timer[c].timer);
    if (time_ms > 0 && r->stop_timer[c].timer) {
        r->stop_timer[c].due_us = esp_timer_get_time() + (int64_t)time_ms * 1000;
        esp_timer_start_once(r->stop_timer[c].timer, (uint64_t)time_ms * 1000);
    }
}

/**
 * @brief SET_POSITION channel @p c to @p pct percent (clamped 0..100). Ignored while blocked.
 */
static void cmd_position(roller_t *r, int c, int pct)
{
    if (r->blocked[c]) {
        return;
    }
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    r->active_ch = c;
    drv_queue_record(r->m, 0x16, 0x00, (uint8_t[]){0, (uint8_t)(c + 1), (uint8_t)pct}, 3);
}

/** @brief tfbus_driver_t::io_new: remember the Lua name of a ROLLER_SHUTTER channel and create its stop timer. */
static void io_new(tfbus_module_t *m, int cls, int ch, const char *name)
{
    roller_t *r = ctx(m);
    if (cls != CLS_ROLLER || ch < 0 || ch >= r->cfg->nch) {
        return;
    }
    strlcpy(r->name[ch], name, sizeof r->name[ch]);
    if (r->stop_timer[ch].timer) {
        return; // channel declared again: keep the existing timer (no leak)
    }
    r->stop_timer[ch].r = r;
    r->stop_timer[ch].ch = ch;
    const esp_timer_create_args_t ta = {.callback = stop_timer_cb, .arg = &r->stop_timer[ch], .name = "rol"};
    if (esp_timer_create(&ta, &r->stop_timer[ch].timer) != ESP_OK) {
        r->stop_timer[ch].timer = NULL;
    }
}

/**
 * @brief tfbus_driver_t::io_set: cache timing features (MaxTime, Up/DownMax, ...) and reconfigure if initialised.
 */
static bool io_set(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int val)
{
    roller_t *r = ctx(m);
    if (cls != CLS_ROLLER || ch < 0 || ch >= r->cfg->nch) {
        return false;
    }
    uint32_t v = (uint32_t)luaL_optinteger(L, val, 0);
    if (idx == r->cfg->maxtime_idx) {
        r->max_ms[ch] = v;
        if (r->initialized && r->cfg->full_init) {
            reconfigure_channel(r, ch);
        }
        return false;
    }
    if (!r->cfg->full_init) {
        return false;
    }
    switch (idx) {
    case F_UPMAX: r->up_ms[ch] = v; break;
    case F_DOWNMAX: r->down_ms[ch] = v; break;
    case F_OFFSET: r->offset_ms[ch] = v; break;
    default: return false;
    }
    if (r->initialized) {
        reconfigure_channel(r, ch);
    }
    return false; // keep generic storage so get() returns it
}

/**
 * @brief tfbus_driver_t::io_execute: MoveUp/MoveDown/Start/Stop/Hold*, SetPosition, Calibration, SetRollerBlocked.
 */
static int io_execute(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int first,
                      int nargs)
{
    roller_t *r = ctx(m);
    if (cls != CLS_ROLLER || ch < 0 || ch >= r->cfg->nch) {
        return -1;
    }
    uint32_t t = nargs > 0 ? (uint32_t)luaL_optinteger(L, first, 0) : 0;
    bool moving = r->state[ch] == ST_UP || r->state[ch] == ST_DOWN;
    switch (idx) {
    case 0: // MoveUp (or stop if moving)
        moving ? cmd_stop(r, ch) : cmd_move(r, ch, ST_UP, t);
        return 0;
    case 1: // MoveDown (or stop if moving)
        moving ? cmd_stop(r, ch) : cmd_move(r, ch, ST_DOWN, t);
        return 0;
    case 2: // Start: reverse of the previous direction
        moving ? cmd_stop(r, ch) : cmd_move(r, ch, r->last_dir[ch] == ST_UP ? ST_DOWN : ST_UP, t);
        return 0;
    case 3: // Stop
        cmd_stop(r, ch);
        return 0;
    case 4: // Hold (direction change)
        cmd_move(r, ch, r->last_dir[ch] == ST_UP ? ST_DOWN : ST_UP, 0);
        return 0;
    case 5: cmd_move(r, ch, ST_UP, 0); return 0;   // HoldUp
    case 6: cmd_move(r, ch, ST_DOWN, 0); return 0; // HoldDown
    case 10: // SetPosition(pos%)
        cmd_position(r, ch, nargs > 0 ? (int)luaL_checkinteger(L, first) : 0);
        return 0;
    case 13: // SetRollerBlocked
        r->blocked[ch] = nargs > 0 && luaL_optinteger(L, first, 0) != 0;
        if (r->blocked[ch]) {
            cmd_stop(r, ch);
        }
        clu_lua_post_number(r->name[ch], F_STATE, r->blocked[ch] ? ST_BLOCKED : ST_STOP);
        return 0;
    case 8: // Calibration(dir): drive fully in that direction for the full travel time
        cmd_move(r, ch, nargs > 0 && luaL_optinteger(L, first, 1) ? ST_UP : ST_DOWN,
                 r->max_ms[ch] + r->max_ms[ch] / 10);
        return 0;
    default:
        ESP_LOGW(TAG, "%s: execute(%d) not implemented", r->name[ch], idx);
        return 0;
    }
}

/** @brief Wire state byte -> logical state (1/0x10 up, 2/0x20 down, else stop). */
static int decode_state(uint8_t s)
{
    switch (s) {
    case 0x01: case 0x10: return ST_UP;
    case 0x02: case 0x20: return ST_DOWN;
    default: return ST_STOP;
    }
}

/**
 * @brief Report a channel state change to Lua (State/Up/Down values; state-change, start, up/down and stop events).
 */
static void publish_state(roller_t *r, int c, int st)
{
    if (r->state[c] == st || !r->name[c][0]) {
        r->state[c] = st;
        return;
    }
    int prev = r->state[c];
    r->state[c] = st;
    const char *n = r->name[c];
    clu_lua_post_number(n, F_STATE, st);
    clu_lua_post_number(n, F_UP, st == ST_UP);
    clu_lua_post_number(n, F_DOWN, st == ST_DOWN);
    clu_lua_post_event(n, EV_STATE);
    if (st == ST_UP || st == ST_DOWN) {
        if (prev == ST_STOP) {
            clu_lua_post_event(n, EV_START);
        }
        clu_lua_post_event(n, st == ST_UP ? EV_UP : EV_DOWN);
    } else if (st == ST_STOP) {
        clu_lua_post_event(n, EV_STOP);
    }
}

/**
 * @brief tfbus_driver_t::on_status: module-wide state byte and per-channel position records.
 */
static void on_status(tfbus_module_t *m, const uint8_t *pl, size_t len)
{
    roller_t *r = ctx(m);
    tfbus_tlv_t rec[6];
    int nr = tfbus_parse_tlv(pl, len, rec, 6);
    int mod_state = ST_STOP;
    for (int i = 0; i < nr; i++) {
        if (rec[i].type == 0x01 && rec[i].len >= 1) {
            mod_state = decode_state(rec[i].data[0]);
        } else if (rec[i].type == 0x16 && rec[i].len >= 3) {
            int c = rec[i].data[0] - 1;
            int pos = rec[i].data[1];
            if (c < 0 || c >= r->cfg->nch || !r->name[c][0]) {
                continue;
            }
            if (pos != r->pos[c]) {
                bool first = r->pos[c] < 0;
                r->pos[c] = pos;
                clu_lua_post_number(r->name[c], F_POSITION, pos);
                if (!first) {
                    clu_lua_post_event(r->name[c], EV_POSITION);
                }
            }
        }
    }
    // The state byte is module-wide: attribute it to the channel we last commanded.
    for (int c = 0; c < r->cfg->nch; c++) {
        if (r->state[c] == ST_BLOCKED || r->blocked[c]) {
            continue;
        }
        publish_state(r, c, c == r->active_ch ? mod_state : ST_STOP);
    }
}

const tfbus_driver_t tfbus_drv_roller_sh_din = {
    .hw_type = 0x17,
    .name = "ROLLER_SH_DIN",
    .cfg = &CFG_DIN1,
    .init = init,
    .deinit = deinit,
    .overdue = overdue,
    .on_status = on_status,
    .io_new = io_new,
    .io_set = io_set,
    .io_execute = io_execute,
};

const tfbus_driver_t tfbus_drv_roller_sh_din3 = {
    .hw_type = 0x2a,
    .name = "ROLLER_SH_DIN_3",
    .cfg = &CFG_DIN3,
    .init = init,
    .deinit = deinit,
    .overdue = overdue,
    .on_status = on_status,
    .io_new = io_new,
    .io_set = io_set,
    .io_execute = io_execute,
};
