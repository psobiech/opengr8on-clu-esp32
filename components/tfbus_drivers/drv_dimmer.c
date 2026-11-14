/**
 * @file drv_dimmer.c
 * @brief DIMMER_MOSFET_DIN (hwType 26 / 0x1a): 1x DIMM (class 5).
 *
 * Interface module_2_0_DIMMER_MOSFET_DIN_fv03_02.xml (fv02 has the same features). Wire format and
 * method behaviour from captures of a real CLU (CLU_2, module addr 1):
 * @verbatim
 *   SetValue / Switch* / Hold : SET_RAMP_TIME 06 [0, ms LE u16, 0, 0]  then
 *                               DIMMER_SET_VALUE 0c [0, level LE u16]   (two frames;
 *                               level 0-1000 = Value * 1000; the module ramps by itself)
 *   timed Switch* expiry      : DIMMER_SET_VALUE only
 *   SetRampTime               : SET_RAMP_TIME at once
 *   SetStartLevel             : 0e [0, level LE u16]
 *   DistributedLogicGroup     : 0f [0, 1, group LE u16]
 *   Min/MaxValue              : nothing (clamped in the CLU)
 *   status                    : 02 | 01 02 10 [level LE u16] | 00 01 01 [inputs]  (level = target)
 * @endverbatim
 * Every call sends its frames, also when the level does not change. The real CLU sends nothing to
 * these modules at startup (refresh capture); OnInit's DIMM:set(0, 0) does the first SetValue.
 *
 * Captured method semantics:
 * - Ramp argument absent = RampTime; given (also 0) = sent as is.
 * - Min/MaxValue clamp; Switch/SwitchOn to MaxValue or (Mode 1) the last value above MinValue,
 *   SwitchOff to MinValue; Time > 0 reverts after Time ms.
 * - HoldValue(Ramp): each call steps 100/Ramp of the full 0-1 range (Ramp 3000 -> 0.033), sends
 *   SET_RAMP_TIME Ramp, clamps to [MinValue, MaxValue]; see light_hold_dir() for the direction.
 * Not checked against a real CLU:
 * - Events: OnValueChange, OnValueRise / OnValueLower, OnSwitchOn / OnSwitchOff (crossing MinValue),
 *   OnOutOfRange.
 * - As module fw 3.0.2 (OM 5.15.24 release notes): the last value is kept in non-volatile memory
 *   (NVS "clu_light" / "d<sn hex>", level 0-1000, written LASTVAL_SAVE_MS after the last change)
 *   and survives a CLU restart.
 * - A status level different from the target (module power cycle) is taken as the new Value.
 * - StatisticState, Load: stored only (nothing sent).
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
enum { F_VALUE = 0, F_RAMP = 1, F_MIN = 2, F_MAX = 3, F_START = 6, F_DLG = 8 };
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
    double start;          // StartLevel
    uint32_t ramp;         // RampTime [ms]
    bool have_target;      // Lua set a value (re-applied by init)
    bool have_status;
    int64_t cmd_at;        // us, last DIMMER_SET_VALUE
    light_timer_t timed;   // Switch* with Time > 0
    double revert_to;
    light_hold_t hold;
    light_timer_t save;    // deferred NVS write of last_on
    unsigned saved;        // last_on level in NVS (0-1000), ~0u = none
} dimm_t;

/** @brief Driver state of @p m, allocated on first use. */
static dimm_t *ctx(tfbus_module_t *module)
{
    if (!module->drv_ctx) {
        dimm_t *dimmer_state = calloc(1, sizeof *dimmer_state);
        if (!dimmer_state) {
            abort(); // out of memory while loading OM.LUA
        }
        dimmer_state->max = 1;
        dimmer_state->last_on = 1;
        dimmer_state->ramp = 500;
        dimmer_state->saved = ~0u;
        nvs_handle_t nvs_handle;
        if (nvs_open(NVS_NS, NVS_READONLY, &nvs_handle) == ESP_OK) {
            char key[16];
            uint32_t lvl;
            snprintf(key, sizeof key, "d%08lx", (unsigned long)module->sn);
            if (nvs_get_u32(nvs_handle, key, &lvl) == ESP_OK && lvl > 0 && lvl <= 1000) {
                dimmer_state->saved = lvl;
                dimmer_state->last_on = lvl / 1000.0;
            }
            nvs_close(nvs_handle);
        }
        module->drv_ctx = dimmer_state;
    }
    return module->drv_ctx;
}

/** @brief Deferred save (bus task): write last_on to NVS if it changed. */
static void save_last(tfbus_module_t *module, void *owner)
{
    dimm_t *dimmer_state = owner;
    unsigned lvl = (unsigned)lround(dimmer_state->last_on * 1000);
    nvs_handle_t nvs_handle;
    if (lvl == dimmer_state->saved || nvs_open(NVS_NS, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        return;
    }
    char key[16];
    snprintf(key, sizeof key, "d%08lx", (unsigned long)module->sn);
    if (nvs_set_u32(nvs_handle, key, lvl) == ESP_OK && nvs_commit(nvs_handle) == ESP_OK) {
        dimmer_state->saved = lvl;
    }
    nvs_close(nvs_handle);
}

/** @brief Remember @p v as the last value the output was on with (Mode LastValue). */
static void set_last(tfbus_module_t *module, dimm_t *dimmer_state, double requested_value)
{
    if (requested_value != dimmer_state->last_on) {
        dimmer_state->last_on = requested_value;
        light_timer_start(&dimmer_state->save, module, dimmer_state, save_last, LASTVAL_SAVE_MS);
    }
}

/** @brief Fire the value events for a change @p old -> @p nv. */
static void events(const dimm_t *dimmer_state, double old, double new_value)
{
    if (!dimmer_state->name[0] || old == new_value) {
        return;
    }
    clu_lua_post_event(dimmer_state->name, EV_CHANGE);
    clu_lua_post_event(dimmer_state->name, new_value > old ? EV_RISE : EV_LOWER);
    if (old <= dimmer_state->min && new_value > dimmer_state->min) {
        clu_lua_post_event(dimmer_state->name, EV_ON);
    } else if (new_value <= dimmer_state->min && old > dimmer_state->min) {
        clu_lua_post_event(dimmer_state->name, EV_OFF);
    }
}

/** @brief Queue a [0, level LE u16] record (0x0c DIMMER_SET_VALUE, 0x0e START_LEVEL). */
static void queue_level(tfbus_module_t *module, uint8_t type, double level)
{
    unsigned lvl = (unsigned)lround(level * 1000);
    drv_queue_record(module, type, 0x00, (uint8_t[]){0, (uint8_t)lvl, (uint8_t)(lvl >> 8)}, 3);
}

/** @brief Queue SET_RAMP_TIME (unless @p ramp is NO_RAMP) + DIMMER_SET_VALUE for the current target. */
#define NO_RAMP UINT32_MAX
static void send_level(tfbus_module_t *module, dimm_t *dimmer_state, uint32_t ramp)
{
    if (ramp != NO_RAMP) {
        light_queue_ramp(module, ramp);
    }
    queue_level(module, 0x0c, dimmer_state->value);
    dimmer_state->cmd_at = esp_timer_get_time();
}

/**
 * @brief Drive the output to @p v (already clamped) and report it.
 * @param L   Lua state when called from a Lua hook (Value stored on the object at @p obj),
 *            NULL from the bus task (Value posted).
 */
static void apply(tfbus_module_t *module, dimm_t *dimmer_state, lua_State *L, int obj, double new_level, uint32_t ramp)
{
    double old = dimmer_state->value;
    dimmer_state->value = new_level;
    dimmer_state->have_target = true;
    if (new_level > dimmer_state->min) {
        set_last(module, dimmer_state, new_level);
    }
    send_level(module, dimmer_state, ramp);
    if (L) {
        tfbus_obj_store(L, obj, F_VALUE, new_level);
    } else if (dimmer_state->name[0]) {
        clu_lua_post_number(dimmer_state->name, F_VALUE, new_level);
    }
    events(dimmer_state, old, new_level);
}

/** @brief Clamp a requested value to [MinValue, MaxValue]; out of range fires OnOutOfRange. */
static double clamp_req(const dimm_t *dimmer_state, double requested_value)
{
    if ((requested_value < dimmer_state->min || requested_value > dimmer_state->max) && dimmer_state->name[0]) {
        clu_lua_post_event(dimmer_state->name, EV_RANGE);
    }
    return light_clamp(requested_value, dimmer_state->min, dimmer_state->max);
}

/** @brief Timed switch expired (bus task): return to the previous value (level only, as the real CLU). */
static void revert(tfbus_module_t *module, void *owner)
{
    dimm_t *dimmer_state = owner;
    apply(module, dimmer_state, NULL, 0, dimmer_state->revert_to, NO_RAMP);
}

/** @brief tfbus_driver_t::deinit: stop the timer. */
static void deinit(tfbus_module_t *module)
{
    dimm_t *dimmer_state = module->drv_ctx;
    light_timer_delete(&dimmer_state->timed);
    light_timer_delete(&dimmer_state->save);
    save_last(module, dimmer_state); // Lua reload / reset: do not lose a pending save
}

/** @brief tfbus_driver_t::overdue: run expired timers whose deferred call was dropped. */
static void overdue(tfbus_module_t *module)
{
    dimm_t *dimmer_state = module->drv_ctx;
    light_timer_overdue(&dimmer_state->timed);
    light_timer_overdue(&dimmer_state->save);
}

/** @brief tfbus_driver_t::io_new: remember the DIMM object's name. */
static void io_new(tfbus_module_t *module, int cls, int channel, const char *name)
{
    if (cls == CLS_DIMM && channel == 0) {
        strlcpy(ctx(module)->name, name, sizeof ctx(module)->name);
    }
}

/** @brief tfbus_driver_t::init: re-apply StartLevel and the level Lua set (module re-addressed / back online). */
static void init(tfbus_module_t *module)
{
    dimm_t *dimmer_state = ctx(module);
    if (dimmer_state->start > 0) {
        queue_level(module, 0x0e, dimmer_state->start);
    }
    if (dimmer_state->have_target) {
        send_level(module, dimmer_state, 0);
    }
}

/**
 * @brief tfbus_driver_t::io_set: Value drives the output, RampTime, StartLevel and
 *        DistributedLogicGroup go to the module at once, Min / MaxValue are cached.
 */
static bool io_set(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel, int idx, int val)
{
    if (cls != CLS_DIMM || channel != 0) {
        return false;
    }
    dimm_t *dimmer_state = ctx(module);
    switch (idx) {
    case F_VALUE:
        light_timer_cancel(&dimmer_state->timed);
        apply(module, dimmer_state, L, obj, clamp_req(dimmer_state, luaL_checknumber(L, val)), dimmer_state->ramp);
        return true;
    case F_RAMP:
        dimmer_state->ramp = (uint32_t)light_clamp(luaL_checknumber(L, val), 0, LIGHT_RAMP_MAX);
        light_queue_ramp(module, dimmer_state->ramp);
        break;
    case F_MIN:
        dimmer_state->min = light_clamp(luaL_checknumber(L, val), 0, 1);
        break;
    case F_MAX:
        dimmer_state->max = light_clamp(luaL_checknumber(L, val), 0, 1);
        break;
    case F_START:
        dimmer_state->start = light_clamp(luaL_checknumber(L, val), 0, 1);
        queue_level(module, 0x0e, dimmer_state->start);
        break;
    case F_DLG:
        light_queue_dlg(module, 0, (uint32_t)light_clamp(luaL_checknumber(L, val), 0, 10000));
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
static int io_execute(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel, int idx, int first,
                      int nargs)
{
    if (cls != CLS_DIMM || channel != 0) {
        return -1;
    }
    dimm_t *dimmer_state = ctx(module);
    double cur = dimmer_state->value;
    if (idx == X_HOLD) {
        uint32_t sweep = light_ramp_arg(L, first, nargs, 0, dimmer_state->ramp);
        light_timer_cancel(&dimmer_state->timed);
        int dir = light_hold_dir(&dimmer_state->hold, esp_timer_get_time(), cur, dimmer_state->min, dimmer_state->max);
        // one step = LIGHT_HOLD_STEP_MS of the sweep, in whole levels (3000 ms -> 33/1000)
        double value_step = sweep ? floor(1000.0 * LIGHT_HOLD_STEP_MS / sweep) / 1000 : 1;
        apply(module, dimmer_state, L, obj, light_clamp(cur + dir * value_step, dimmer_state->min, dimmer_state->max), sweep);
        return 0;
    }
    if (idx != X_SWITCH && idx != X_ON && idx != X_OFF) {
        return -1;
    }
    double ms = light_arg(L, first, nargs, 0, 0);
    uint32_t ramp = light_ramp_arg(L, first, nargs, 1, dimmer_state->ramp);
    bool last = idx != X_OFF && light_arg(L, first, nargs, 2, 0) == 1;
    double on_level = last && dimmer_state->last_on > dimmer_state->min ? light_clamp(dimmer_state->last_on, dimmer_state->min, dimmer_state->max) : dimmer_state->max;
    double output_level = idx == X_ON ? on_level : idx == X_OFF ? dimmer_state->min : (cur > dimmer_state->min ? dimmer_state->min : on_level);
    light_timer_cancel(&dimmer_state->timed);
    if (ms > 0 && output_level != cur) {
        dimmer_state->revert_to = cur;
        light_timer_start(&dimmer_state->timed, module, dimmer_state, revert, (uint32_t)ms);
    }
    apply(module, dimmer_state, L, obj, output_level, ramp);
    return 0;
}

/** @brief tfbus_driver_t::on_status: a level other than the target (after the hold-off) is taken over. */
static void on_status(tfbus_module_t *module, const uint8_t *status_payload, size_t len)
{
    dimm_t *dimmer_state = ctx(module);
    tfbus_tlv_t status_records[4];
    int status_record_count = tfbus_parse_tlv(status_payload, len, status_records, 4);
    for (int i = 0; i < status_record_count; i++) {
        if (status_records[i].type != 0x01 || status_records[i].len < 2) {
            continue;
        }
        unsigned lvl = status_records[i].data[0] | status_records[i].data[1] << 8;
        double reported_level = light_clamp(lvl / 1000.0, 0, 1);
        bool first = !dimmer_state->have_status;
        dimmer_state->have_status = true;
        if (first && !dimmer_state->have_target) {
            // Nothing requested yet: take the module's level silently.
            dimmer_state->value = reported_level;
            if (reported_level > dimmer_state->min) {
                set_last(module, dimmer_state, reported_level);
            }
            if (dimmer_state->name[0]) {
                clu_lua_post_number(dimmer_state->name, F_VALUE, reported_level);
            }
            return;
        }
        if (lvl == (unsigned)lround(dimmer_state->value * 1000) ||
            esp_timer_get_time() - dimmer_state->cmd_at < STATUS_HOLDOFF_US) {
            return;
        }
        double old = dimmer_state->value;
        dimmer_state->value = reported_level;
        if (dimmer_state->name[0]) {
            clu_lua_post_number(dimmer_state->name, F_VALUE, reported_level);
        }
        events(dimmer_state, old, reported_level);
        return;
    }
}

const tfbus_driver_t tfbus_drv_dimmer_mosfet_din = {
    .hw_type = 0x1a,
    .name = "DIMMER_MOSFET_DIN",
    .init = init,
    .deinit = deinit,
    .overdue = overdue,
    .on_status = on_status,
    .io_new = io_new,
    .io_set = io_set,
    .io_execute = io_execute,
};
