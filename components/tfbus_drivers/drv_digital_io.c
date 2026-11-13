/**
 * @file drv_digital_io.c
 * @brief Digital output/input modules (shared implementation).
 *
 * - IO_MODULE_DIN_8 (hwType 30 / 0x1e): 8x DOUT (class 4), 8x DIN (class 3), PowerSupplyVoltage (12).
 * - RELAY_DIN_4 (hwType 21 / 0x15): 4x DOUT (class 4).
 *
 * RELAY_DIN_4 init per ch (real CLU capture):
 * @verbatim
 *   DEACTIVATE[1<<ch], CHANNEL_MAP[ch,0x02], MODULE_PARAM[ch,1,0,0]; then CHANNEL_MAP[ch,0x02] descending.
 * @endverbatim
 *
 * IO_MODULE_DIN_8 wire format from docs/TFBUS_IO_MODULE_DIN_8.md, verified against a real CLU
 * startup capture:
 * @verbatim
 *   init  : per ch DEACTIVATE[1<<ch] + MODULE_PARAM[ch,1,0,0]; per ch CHANNEL_INIT[0,ch,0] + MODULE_PARAM[ch,0,0,0]
 *   output: ACTIVATE[1<<ch] / DEACTIVATE[1<<ch]
 *   status: 03 | 00 01 01 [inputs] | 01 01 01 [outputs] | 02 02 10 [temp LE 0.01C]
 * @endverbatim
 *
 * Timed Switch/SwitchOn/SwitchOff(ms) revert the output with an esp_timer whose work is deferred
 * to the bus task.
 */
#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include "tfbus_drivers.h"
#include "tfbus_master.h"


#define CH 8 // max channels

/** @brief Per-variant parameters. */
typedef struct {
    int ndout, ndin;
    bool relay; // RELAY_DIN_x init style (CHANNEL_MAP per output)
} dio_cfg_t;

static const dio_cfg_t CFG_IO8 = {.ndout = 8, .ndin = 8};
static const dio_cfg_t CFG_RELAY4 = {.ndout = 4, .ndin = 0, .relay = true};

/** @brief OM object classes hosted on these modules. */
enum { CLS_DIN = 3, CLS_DOUT = 4 };

/** @brief DIN events (interface module_2_0_IO_MODULE_DIN_8_fv02_02.xml). */
enum { EV_CHANGE = 0, EV_ON = 1, EV_OFF = 2, EV_SHORT = 3, EV_LONG = 4, EV_HOLD = 5, EV_CLICK = 6 };

typedef struct io8 io8_t;

/** @brief Timed output: reverts the channel to `revert_to` when the timer expires. */
typedef struct {
    tfbus_module_t *m;
    int ch;
    bool revert_to; // output state when the timer expires
    volatile uint32_t gen; // bumped on every start/cancel: a fire queued before is stale
    int64_t due_us;        // revert deadline [esp_timer us], 0 = none (driver lock)
    esp_timer_handle_t timer;
} dout_timer_t;

/** @brief Driver state per module (drv_ctx). */
struct io8 {
    const dio_cfg_t *cfg;
    char dout[CH][24];
    char din[CH][24];
    uint16_t hold_delay[CH];     // ms
    uint16_t hold_interval[CH];  // ms
    int64_t pressed_at[CH];      // us, 0 = released
    int64_t next_hold[CH];       // us
    uint8_t outputs;             // last reported output bitmask
    uint8_t desired;             // requested by Lua (applied by init after re-addressing)
    uint8_t inputs;
    bool have_status;
    dout_timer_t timed[CH];      // Switch/SwitchOn/SwitchOff(time)
};

/** @brief Driver state of @p m, allocated on first use. */
static io8_t *ctx(tfbus_module_t *m)
{
    if (!m->drv_ctx) {
        io8_t *c = calloc(1, sizeof *c);
        if (!c) {
            abort(); // out of memory while loading OM.LUA: reboot instead of a NULL dereference
        }
        c->cfg = m->drv->cfg;
        for (int i = 0; i < CH; i++) {
            c->hold_delay[i] = 500;
            c->hold_interval[i] = 100;
        }
        m->drv_ctx = c;
    }
    return m->drv_ctx;
}

static void set_output(tfbus_module_t *m, int ch, bool on);

/**
 * @brief Timed output expired: revert the channel unless the timer was restarted / cancelled (@p gen).
 *
 * Bus task (deferred), under the driver lock: Lua cannot start/cancel concurrently.
 */
static void timed_fire(tfbus_module_t *m, void *arg, uint32_t gen)
{
    dout_timer_t *t = arg;
    if (gen == t->gen && t->due_us) {
        t->due_us = 0;
        set_output(m, t->ch, t->revert_to);
    }
}

/**
 * @brief esp_timer callback: do no work here (no blocking queue send, no shared state), hand it over.
 */
static void timed_cb(void *arg)
{
    dout_timer_t *t = arg;
    tfbus_master_defer(timed_fire, t->m, t, t->gen);
}

/** @brief Cancel a channel's timed output (a fire already queued becomes stale). */
static void timed_cancel(io8_t *c, int ch)
{
    c->timed[ch].gen++;
    c->timed[ch].due_us = 0;
    if (c->timed[ch].timer) {
        esp_timer_stop(c->timed[ch].timer);
    }
}

/** @brief tfbus_driver_t::overdue: revert outputs whose timer expired but whose deferred call was dropped. */
static void overdue(tfbus_module_t *m)
{
    io8_t *c = m->drv_ctx;
    int64_t now = esp_timer_get_time();
    for (int ch = 0; ch < CH; ch++) {
        dout_timer_t *t = &c->timed[ch];
        if (t->due_us && now >= t->due_us) {
            t->due_us = 0;
            set_output(m, t->ch, t->revert_to);
        }
    }
}

/** @brief tfbus_driver_t::deinit: stop and delete the output timers. */
static void deinit(tfbus_module_t *m)
{
    io8_t *c = m->drv_ctx;
    for (int ch = 0; ch < CH; ch++) {
        if (c->timed[ch].timer) {
            esp_timer_stop(c->timed[ch].timer);
            esp_timer_delete(c->timed[ch].timer);
            c->timed[ch].timer = NULL;
        }
    }
}

/** @brief Start (or restart) a channel's timed output: revert to @p revert_to after @p ms. */
static void timed_start(tfbus_module_t *m, io8_t *c, int ch, bool revert_to, uint32_t ms)
{
    dout_timer_t *t = &c->timed[ch];
    if (!t->timer) {
        t->m = m;
        t->ch = ch;
        const esp_timer_create_args_t a = {.callback = timed_cb, .arg = t, .name = "dout"};
        if (esp_timer_create(&a, &t->timer) != ESP_OK) {
            return;
        }
    }
    t->revert_to = revert_to;
    esp_timer_stop(t->timer);
    t->gen++;
    t->due_us = esp_timer_get_time() + (int64_t)ms * 1000;
    esp_timer_start_once(t->timer, (uint64_t)ms * 1000);
}

/** @brief tfbus_driver_t::io_new: remember the Lua name of a DIN / DOUT channel. */
static void io_new(tfbus_module_t *m, int cls, int ch, const char *name)
{
    io8_t *c = ctx(m);
    if (ch < 0 || ch >= CH) {
        return;
    }
    if (cls == CLS_DOUT) {
        strlcpy(c->dout[ch], name, sizeof c->dout[ch]);
    } else if (cls == CLS_DIN) {
        strlcpy(c->din[ch], name, sizeof c->din[ch]);
    }
}

/**
 * @brief tfbus_driver_t::init: queue the channel init sequence and re-apply the outputs Lua requested.
 */
static void init(tfbus_module_t *m)
{
    const dio_cfg_t *cfg = ctx(m)->cfg;
    for (int ch = 0; ch < cfg->ndout; ch++) {
        bool on = ctx(m)->desired & (1 << ch); // real CLU sends DEACTIVATE (all off at boot)
        drv_queue_record(m, on ? 0x08 : 0x09, 0x01, (uint8_t[]){(uint8_t)(1 << ch)}, 1);
        if (cfg->relay) {
            drv_queue_record(m, 0x0b, 0x00, (uint8_t[]){(uint8_t)ch, 0x02}, 2);       // CHANNEL_MAP
        }
        drv_queue_record(m, 0x0f, 0x00, (uint8_t[]){(uint8_t)ch, 1, 0, 0}, 4);          // MODULE_PARAM out
    }
    if (cfg->relay) {
        for (int ch = cfg->ndout - 1; ch >= 0; ch--) {
            drv_queue_record(m, 0x0b, 0x00, (uint8_t[]){(uint8_t)ch, 0x02}, 2);
        }
    }
    for (int ch = 0; ch < cfg->ndin; ch++) {
        drv_queue_record(m, 0x04, 0x00, (uint8_t[]){0, (uint8_t)ch, 0}, 3);             // CHANNEL_INIT
        drv_queue_record(m, 0x0f, 0x00, (uint8_t[]){(uint8_t)ch, 0, 0, 0}, 4);          // MODULE_PARAM in
    }
}

/** @brief Queue ACTIVATE / DEACTIVATE for one output channel. */
static void set_output(tfbus_module_t *m, int ch, bool on)
{
    io8_t *c = ctx(m);
    c->desired = on ? (c->desired | (1 << ch)) : (c->desired & ~(1 << ch));
    drv_queue_record(m, on ? 0x08 : 0x09, 0x01, (uint8_t[]){(uint8_t)(1 << ch)}, 1);
}

/**
 * @brief tfbus_driver_t::io_set: DOUT Value (0) drives the output; DIN HoldDelay (2) / HoldInterval (3) are cached.
 */
static bool io_set(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int val)
{
    io8_t *c = ctx(m);
    if (ch < 0 || ch >= CH) {
        return false;
    }
    if (cls == CLS_DOUT && idx == 0) { // Value
        bool on = lua_toboolean(L, val) && !(lua_isnumber(L, val) && lua_tonumber(L, val) == 0);
        timed_cancel(c, ch);
        set_output(m, ch, on);
        tfbus_obj_store(L, obj, 0, on);
        return true;
    }
    if (cls == CLS_DIN && idx == 2) {
        c->hold_delay[ch] = (uint16_t)luaL_optinteger(L, val, 500);
    } else if (cls == CLS_DIN && idx == 3) {
        c->hold_interval[ch] = (uint16_t)luaL_optinteger(L, val, 100);
    }
    return false; // store generically
}

/**
 * @brief tfbus_driver_t::io_execute: DOUT Switch (0) / SwitchOn (1) / SwitchOff (2) with optional time [ms].
 *
 * Time > 0: output takes the new state for that time, then returns to the previous one.
 */
static int io_execute(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int first,
                      int nargs)
{
    if (cls != CLS_DOUT || ch < 0 || ch >= CH) {
        return -1;
    }
    lua_getfield(L, obj, "_f");
    lua_rawgeti(L, -1, 0);
    bool cur = lua_tointeger(L, -1) != 0;
    lua_pop(L, 2);

    bool on;
    switch (idx) {
    case 0: on = !cur; break; // Switch
    case 1: on = true; break; // SwitchOn
    case 2: on = false; break; // SwitchOff
    default: return -1;
    }
    // Time [ms] > 0: output takes the new state for that time, then returns to the previous one.
    lua_Integer ms = nargs > 0 ? luaL_optinteger(L, first, 0) : 0;
    io8_t *c = ctx(m);
    timed_cancel(c, ch);
    if (ms > 0 && on != cur) {
        timed_start(m, c, ch, cur, (uint32_t)ms);
    }
    set_output(m, ch, on);
    tfbus_obj_store(L, obj, 0, on);
    return 0;
}

/**
 * @brief Turn input bitmask changes into DIN values and events (change, on/off, click/short/long, hold).
 */
static void din_edges(tfbus_module_t *m, io8_t *c, uint8_t inputs)
{
    int64_t now = esp_timer_get_time();
    for (int ch = 0; ch < CH; ch++) {
        const char *n = c->din[ch];
        bool was = c->inputs & (1 << ch);
        bool is = inputs & (1 << ch);
        if (!n[0]) {
            continue;
        }
        if (is != was) {
            clu_lua_post_number(n, 0, is);
            clu_lua_post_event(n, EV_CHANGE);
            clu_lua_post_event(n, is ? EV_ON : EV_OFF);
            if (is) {
                c->pressed_at[ch] = now;
                c->next_hold[ch] = now + (int64_t)c->hold_delay[ch] * 1000;
            } else if (c->pressed_at[ch]) {
                int64_t ms = (now - c->pressed_at[ch]) / 1000;
                clu_lua_post_event(n, ms < 500 ? EV_CLICK : ms < 2000 ? EV_SHORT : EV_LONG);
                c->pressed_at[ch] = 0;
            }
        } else if (is && c->pressed_at[ch] && now >= c->next_hold[ch]) {
            clu_lua_post_event(n, EV_HOLD);
            c->next_hold[ch] = now + (int64_t)c->hold_interval[ch] * 1000;
        }
    }
    c->inputs = inputs;
}

/**
 * @brief Report output bitmask changes to the DOUT objects (Value + OnValueChange / OnSwitchOn / OnSwitchOff).
 */
static void dout_report(io8_t *c, uint8_t outputs)
{
    for (int ch = 0; ch < CH; ch++) {
        bool was = c->outputs & (1 << ch);
        bool is = outputs & (1 << ch);
        if (is != was && c->dout[ch][0]) {
            clu_lua_post_number(c->dout[ch], 0, is);
            clu_lua_post_event(c->dout[ch], 0);          // OnValueChange
            clu_lua_post_event(c->dout[ch], is ? 1 : 2); // OnSwitchOn / OnSwitchOff
        }
    }
    c->outputs = outputs;
}

/** @brief tfbus_driver_t::on_status: track inputs / outputs; the first status is taken without edge events. */
static void on_status(tfbus_module_t *m, const uint8_t *pl, size_t len)
{
    io8_t *c = ctx(m);
    tfbus_tlv_t r[4];
    int n = tfbus_parse_tlv(pl, len, r, 4);
    uint8_t inputs = c->inputs, outputs = c->outputs;
    for (int i = 0; i < n; i++) {
        if (r[i].type == 0x00 && r[i].len >= 1) {
            inputs = r[i].data[0];
        } else if (r[i].type == 0x01 && r[i].len >= 1) {
            outputs = r[i].data[0];
        }
    }
    if (!c->have_status) {
        // First status after boot: take state silently (no edge events), publish values.
        c->have_status = true;
        c->inputs = inputs;
        c->outputs = outputs;
        for (int ch = 0; ch < CH; ch++) {
            if (c->din[ch][0]) {
                clu_lua_post_number(c->din[ch], 0, (inputs >> ch) & 1);
            }
            if (c->dout[ch][0]) {
                clu_lua_post_number(c->dout[ch], 0, (outputs >> ch) & 1);
            }
        }
        return;
    }
    din_edges(m, c, inputs);
    if (outputs != c->outputs) {
        dout_report(c, outputs);
    }
}

const tfbus_driver_t tfbus_drv_relay_din4 = {
    .hw_type = 0x15,
    .name = "RELAY_DIN_4",
    .cfg = &CFG_RELAY4,
    .init = init,
    .deinit = deinit,
    .overdue = overdue,
    .on_status = on_status,
    .io_new = io_new,
    .io_set = io_set,
    .io_execute = io_execute,
};

const tfbus_driver_t tfbus_drv_io_module_din8 = {
    .hw_type = 0x1e,
    .name = "IO_MODULE_DIN_8",
    .cfg = &CFG_IO8,
    .init = init,
    .deinit = deinit,
    .overdue = overdue,
    .on_status = on_status,
    .io_new = io_new,
    .io_set = io_set,
    .io_execute = io_execute,
};
