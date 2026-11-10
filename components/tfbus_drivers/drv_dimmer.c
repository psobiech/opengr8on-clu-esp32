/**
 * @file drv_dimmer.c
 * @brief DIMMER_MOSFET_DIN (hwType 26 / 0x1a): 1x DIMM (class 5).
 *
 * Interface module_2_0_DIMMER_MOSFET_DIN_fv03_02.xml. Wire format from captures of a real CLU
 * (clu firmware/tfbus_capture_20261014_180929.log, CLU_2 addr 1; docs/TFBUS_DIMMER_MOSFET_DIN.md):
 * @verbatim
 *   SetValue: SET_RAMP_TIME[0, ms LE u16, 0, 0]  then  DIMMER_SET_VALUE 0x0c [0, level LE u16]
 *             (two frames; level 0-1000 = Value * 1000; the module ramps by itself)
 *   status  : 02 | 01 02 10 [level LE u16] | 00 01 01 [inputs]   (level = programmed target)
 * @endverbatim
 * The real CLU sends nothing to these modules at startup (refresh capture); OnInit's
 * DIMM:set(0, 0) does the first SetValue.
 *
 * Done in the CLU (semantics from the interface, not checked against a real CLU):
 * - Min/MaxValue clamp + OnOutOfRange; Switch/SwitchOn to MaxValue or (Mode 1) the last value
 *   above MinValue, SwitchOff to MinValue; Time > 0 reverts after Time ms.
 * - As module fw 3.0.2 (OM 5.15.24 release notes): Switch* Ramp 0 = RampTime (SetValue / Hold:
 *   0 = instant); the last value is kept in non-volatile memory (NVS "clu_light" / "d<sn hex>",
 *   level 0-1000, written LASTVAL_SAVE_MS after the last change) and survives a CLU restart.
 * - HoldValue steps the value on each call (see light_hold_step()); Ramp = full sweep time.
 * - Events: OnValueChange, OnValueRise / OnValueLower, OnSwitchOn / OnSwitchOff (crossing MinValue).
 * - A status level different from the target (module power cycle) is taken as the new Value.
 * - StartLevel, StatisticState, Load, DistributedLogicGroup: stored only (wire format unknown).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "drv_light.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include "nvs.h"
#include "tfbus_drivers.h"
#include "tfbus_master.h"

#define CLS_DIMM 5

/** @brief DIMM feature indices. */
enum { F_VALUE = 0, F_RAMP = 1, F_MIN = 2, F_MAX = 3 };
/** @brief DIMM events. */
enum { EV_CHANGE = 0, EV_ON = 1, EV_OFF = 2, EV_RISE = 3, EV_LOWER = 4, EV_RANGE = 5 };
/** @brief DIMM execute methods. */
enum { X_HOLD = 0, X_SWITCH = 1, X_ON = 2, X_OFF = 3 };

/** @brief After a command, status replies still showing the old level are ignored this long. */
#define STATUS_HOLDOFF_US 1000000

/** @brief Last value written to NVS this long after its last change (HoldValue: no flash wear). */
#define LASTVAL_SAVE_MS 5000
#define NVS_NS "clu_light"

/** @brief Driver state per module (drv_ctx). */
typedef struct {
    char name[24];         // DIMM object (empty = not declared)
    double value;          // target 0..1
    double last_on;        // last value above MinValue (Mode LastValue)
    double min, max;
    uint32_t ramp;         // RampTime [ms]
    bool have_target;      // Lua set a value (re-applied by init)
    bool have_status;
    int64_t cmd_at;        // us, last DIMMER_SET_VALUE
    light_timer_t timed;   // Switch* with Time > 0
    double revert_to;
    uint32_t revert_ramp;
    light_hold_t hold;
    light_timer_t save;    // deferred NVS write of last_on
    unsigned saved;        // last_on level in NVS (0-1000), ~0u = none
} dimm_t;

/** @brief Driver state of @p m, allocated on first use. */
static dimm_t *ctx(tfbus_module_t *m)
{
    if (!m->drv_ctx) {
        dimm_t *c = calloc(1, sizeof *c);
        if (!c) {
            abort(); // out of memory while loading OM.LUA
        }
        c->max = 1;
        c->last_on = 1;
        c->ramp = 500;
        c->saved = ~0u;
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            char key[16];
            uint32_t lvl;
            snprintf(key, sizeof key, "d%08lx", (unsigned long)m->sn);
            if (nvs_get_u32(h, key, &lvl) == ESP_OK && lvl > 0 && lvl <= 1000) {
                c->saved = lvl;
                c->last_on = lvl / 1000.0;
            }
            nvs_close(h);
        }
        m->drv_ctx = c;
    }
    return m->drv_ctx;
}

/** @brief Deferred save (bus task): write last_on to NVS if it changed. */
static void save_last(tfbus_module_t *m, void *owner)
{
    dimm_t *c = owner;
    unsigned lvl = (unsigned)lround(c->last_on * 1000);
    nvs_handle_t h;
    if (lvl == c->saved || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char key[16];
    snprintf(key, sizeof key, "d%08lx", (unsigned long)m->sn);
    if (nvs_set_u32(h, key, lvl) == ESP_OK && nvs_commit(h) == ESP_OK) {
        c->saved = lvl;
    }
    nvs_close(h);
}

/** @brief Remember @p v as the last value the output was on with (Mode LastValue). */
static void set_last(tfbus_module_t *m, dimm_t *c, double v)
{
    if (v != c->last_on) {
        c->last_on = v;
        light_timer_start(&c->save, m, c, save_last, LASTVAL_SAVE_MS);
    }
}

/** @brief Fire the value events for a change @p old -> @p nv. */
static void events(const dimm_t *c, double old, double nv)
{
    if (!c->name[0] || old == nv) {
        return;
    }
    clu_lua_post_event(c->name, EV_CHANGE);
    clu_lua_post_event(c->name, nv > old ? EV_RISE : EV_LOWER);
    if (old <= c->min && nv > c->min) {
        clu_lua_post_event(c->name, EV_ON);
    } else if (nv <= c->min && old > c->min) {
        clu_lua_post_event(c->name, EV_OFF);
    }
}

/** @brief Queue SET_RAMP_TIME + DIMMER_SET_VALUE for the current target. */
static void send_level(tfbus_module_t *m, dimm_t *c, uint32_t ramp)
{
    unsigned lvl = (unsigned)lround(c->value * 1000);
    light_queue_ramp(m, ramp);
    drv_queue_record(m, 0x0c, 0x00, (uint8_t[]){0, (uint8_t)lvl, (uint8_t)(lvl >> 8)}, 3);
    c->cmd_at = esp_timer_get_time();
}

/**
 * @brief Drive the output to @p v (already clamped) and report it.
 * @param L   Lua state when called from a Lua hook (Value stored on the object at @p obj),
 *            NULL from the bus task (Value posted).
 */
static void apply(tfbus_module_t *m, dimm_t *c, lua_State *L, int obj, double v, uint32_t ramp)
{
    double old = c->value;
    c->value = v;
    c->have_target = true;
    if (v > c->min) {
        set_last(m, c, v);
    }
    send_level(m, c, ramp);
    if (L) {
        tfbus_obj_store(L, obj, F_VALUE, v);
    } else if (c->name[0]) {
        clu_lua_post_number(c->name, F_VALUE, v);
    }
    events(c, old, v);
}

/** @brief Clamp a requested value to [MinValue, MaxValue]; out of range fires OnOutOfRange. */
static double clamp_req(const dimm_t *c, double v)
{
    if ((v < c->min || v > c->max) && c->name[0]) {
        clu_lua_post_event(c->name, EV_RANGE);
    }
    return light_clamp(v, c->min, c->max);
}

/** @brief Timed switch expired (bus task): return to the previous value. */
static void revert(tfbus_module_t *m, void *owner)
{
    dimm_t *c = owner;
    apply(m, c, NULL, 0, c->revert_to, c->revert_ramp);
}

/** @brief tfbus_driver_t::deinit: stop the timer. */
static void deinit(tfbus_module_t *m)
{
    dimm_t *c = m->drv_ctx;
    light_timer_delete(&c->timed);
    light_timer_delete(&c->save);
    save_last(m, c); // Lua reload / reset: do not lose a pending save
}

/** @brief tfbus_driver_t::io_new: remember the DIMM object's name. */
static void io_new(tfbus_module_t *m, int cls, int ch, const char *name)
{
    if (cls == CLS_DIMM && ch == 0) {
        strlcpy(ctx(m)->name, name, sizeof ctx(m)->name);
    }
}

/** @brief tfbus_driver_t::init: re-apply the level Lua set (module re-addressed / back online). */
static void init(tfbus_module_t *m)
{
    dimm_t *c = ctx(m);
    if (c->have_target) {
        send_level(m, c, 0);
    }
}

/** @brief tfbus_driver_t::io_set: Value drives the output; RampTime / Min / MaxValue are cached. */
static bool io_set(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int val)
{
    if (cls != CLS_DIMM || ch != 0) {
        return false;
    }
    dimm_t *c = ctx(m);
    switch (idx) {
    case F_VALUE:
        light_timer_cancel(&c->timed);
        apply(m, c, L, obj, clamp_req(c, luaL_checknumber(L, val)), c->ramp);
        return true;
    case F_RAMP:
        c->ramp = (uint32_t)light_clamp(luaL_checknumber(L, val), 0, LIGHT_RAMP_MAX);
        break;
    case F_MIN:
        c->min = light_clamp(luaL_checknumber(L, val), 0, 1);
        break;
    case F_MAX:
        c->max = light_clamp(luaL_checknumber(L, val), 0, 1);
        break;
    default:
        break;
    }
    return false; // store generically
}

/**
 * @brief tfbus_driver_t::io_execute: HoldValue(Ramp), Switch / SwitchOn(Time, Ramp, Mode),
 *        SwitchOff(Time, Ramp).
 */
static int io_execute(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int first,
                      int nargs)
{
    if (cls != CLS_DIMM || ch != 0) {
        return -1;
    }
    dimm_t *c = ctx(m);
    double cur = c->value;
    if (idx == X_HOLD) {
        uint32_t sweep = light_ramp_arg(L, first, nargs, 0, c->ramp);
        light_timer_cancel(&c->timed);
        uint32_t step = light_hold_step(&c->hold, esp_timer_get_time(), cur, c->min, c->max);
        double d = sweep ? (c->max - c->min) * step / sweep : 1;
        double v = light_clamp(cur + c->hold.dir * d, c->min, c->max);
        if (v != cur) {
            apply(m, c, L, obj, v, step);
        }
        return 0;
    }
    if (idx != X_SWITCH && idx != X_ON && idx != X_OFF) {
        return -1;
    }
    double ms = light_arg(L, first, nargs, 0, 0);
    uint32_t ramp = light_ramp_arg(L, first, nargs, 1, 0);
    if (ramp == 0) {
        ramp = c->ramp; // fw 3.0.2: Switch* Ramp 0 = RampTime
    }
    bool last = idx != X_OFF && light_arg(L, first, nargs, 2, 0) == 1;
    double on_level = last && c->last_on > c->min ? light_clamp(c->last_on, c->min, c->max) : c->max;
    double v = idx == X_ON ? on_level : idx == X_OFF ? c->min : (cur > c->min ? c->min : on_level);
    light_timer_cancel(&c->timed);
    if (ms > 0 && v != cur) {
        c->revert_to = cur;
        c->revert_ramp = ramp;
        light_timer_start(&c->timed, m, c, revert, (uint32_t)ms);
    }
    apply(m, c, L, obj, v, ramp);
    return 0;
}

/** @brief tfbus_driver_t::on_status: a level other than the target (after the hold-off) is taken over. */
static void on_status(tfbus_module_t *m, const uint8_t *pl, size_t len)
{
    dimm_t *c = ctx(m);
    tfbus_tlv_t r[4];
    int n = tfbus_parse_tlv(pl, len, r, 4);
    for (int i = 0; i < n; i++) {
        if (r[i].type != 0x01 || r[i].len < 2) {
            continue;
        }
        unsigned lvl = r[i].data[0] | r[i].data[1] << 8;
        double v = light_clamp(lvl / 1000.0, 0, 1);
        bool first = !c->have_status;
        c->have_status = true;
        if (first && !c->have_target) {
            // Nothing requested yet: take the module's level silently.
            c->value = v;
            if (v > c->min) {
                set_last(m, c, v);
            }
            if (c->name[0]) {
                clu_lua_post_number(c->name, F_VALUE, v);
            }
            return;
        }
        if (lvl == (unsigned)lround(c->value * 1000) ||
            esp_timer_get_time() - c->cmd_at < STATUS_HOLDOFF_US) {
            return;
        }
        double old = c->value;
        c->value = v;
        if (c->name[0]) {
            clu_lua_post_number(c->name, F_VALUE, v);
        }
        events(c, old, v);
        return;
    }
}

const tfbus_driver_t tfbus_drv_dimmer_mosfet_din = {
    .hw_type = 0x1a,
    .name = "DIMMER_MOSFET_DIN",
    .init = init,
    .deinit = deinit,
    .on_status = on_status,
    .io_new = io_new,
    .io_set = io_set,
    .io_execute = io_execute,
};
