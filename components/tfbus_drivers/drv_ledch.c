/**
 * @file drv_ledch.c
 * @brief LED_RGBW_DIN channel objects: LED_CHANNEL (class 49, x4) and LED_CCT (class 72, x2).
 *
 * Interface module_2_0_LED_RGBW_DIN_fv04_02.xml. Wire format and method behaviour from captures of
 * a real CLU (CLU_2, module addr 5); every call sends its frames, also when nothing changes:
 * @verbatim
 *   output of channel ch : one frame  02 | 06 05 00 [ch, t LE u16, 0, 0] | 05 02 10 [ch, value]
 *                          (t = SET_RAMP_TIME in 10 ms)
 *   Hold start           : one frame per channel  02 | 06 05 00 [ch, t, 0, 0] | 13 03 00 [ch, dir, target]
 *                          (dir 1 up / 2 down; the module ramps towards target itself)
 *   Hold stop            : 13 03 00 [ch, 0, 0] per channel, in one frame
 *   DistributedLogicGroup: 0f 04 00 [ch, 1, group LE u16]
 *   RampTime, Min/MaxValue, StatisticState, Load: nothing sent
 *   status (after a change): n | 04 02 08 [ch, value] ...   (the channels' new values)
 * @endverbatim
 *
 * LED_CHANNEL n drives channel n (0 R, 1 G, 2 B, 3 W):
 * - SetValue(v, Ramp) clamps to [MinValue, MaxValue] (truncated to an integer); Ramp absent =
 *   RampTime. The ramp goes out in whole seconds: t = round(Ramp / 1000 ms) * 100.
 * - SwitchOn -> MaxValue, or (Mode 1) the last value above 0; SwitchOff -> 0; Switch toggles; Time > 0
 *   reverts after Time ms.
 * - HoldValue / HoldValueUp / HoldValueDown run on the module: the first call sends Hold start
 *   (towards MaxValue / MinValue, t = Ramp), later calls only keep it going; Hold stop follows
 *   CH_HOLD_STOP_MS after the last call. HoldValue alternates the direction per hold (light_hold_dir()).
 * - Value is what the module reports (the CLU's get() lags the same way); events follow it.
 *
 * LED_CCT n drives warm channel 2n and cold channel 2n+1:
 * - p = warm share = ColorTemp% / 100 = (6500 - ColorTemp) / 3800; warm = round(Value * p),
 *   cold = Value - warm. Default 4600 K (50 %).
 * - Each channel gets its own ramp so both finish together: t = Ramp/10 * D * 255 / dch in float,
 *   D = change of Value / 255 (SetValue, Switch*) or of p (SetColorTemp*), dch = change of the channel
 *   from the value the module reported last (0 -> 255). Captured: exact multiples on the Value path
 *   come out one lower (floor(x - eps)). Ramp rounded to whole seconds as LED_CHANNEL (assumed).
 * - HoldValue*: both channels towards round(MaxValue * p) / the rest (up) or 0 (down). HoldColorTemp*:
 *   warm towards Value and cold towards 0 (warmer) or the reverse. Hold stop CCT_HOLD_STOP_MS after
 *   the last call; Value / ColorTemp then follow what the module reports.
 * - ColorTemp, ColorTemp% and Value change at once; WarmValue / ColdValue follow the module.
 * Not checked against a real CLU: the events, LED_CCT MinValue on SwitchOff.
 */
#include "drv_ledch.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "drv_light.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include "tfbus_drivers.h"

/** @brief LED_CHANNEL features / methods / events. */
enum { C_VALUE = 0, C_RAMP = 1, C_MAX = 2, C_MIN = 3, C_DLG = 4 };
enum { CX_SET = 0, CX_ON = 1, CX_OFF = 2, CX_SWITCH = 3, CX_HOLD = 4, CX_UP = 5, CX_DOWN = 6 };
enum { CE_CHANGE = 0, CE_ON = 1, CE_OFF = 2, CE_RISE = 3, CE_LOWER = 4, CE_RANGE = 5 };

/** @brief LED_CCT features / methods / events. */
enum { T_CT = 0, T_PCT = 1, T_VALUE = 2, T_WARM = 3, T_COLD = 4, T_RAMP = 5, T_MAX = 6, T_MIN = 7 };
enum { TX_CT = 0, TX_PCT = 1, TX_VALUE = 2, TX_SWITCH = 3, TX_ON = 4, TX_OFF = 5, TX_HOLD = 6,
       TX_UP = 7, TX_DOWN = 8, TX_HOLD_CT = 9, TX_WARM = 10, TX_COOL = 11 };
enum { TE_CHANGE = 0, TE_CT = 1, TE_ON = 2, TE_OFF = 3, TE_RANGE = 4 };

/** @brief Hold stop after the last Hold* call [ms] (captured: ~520-575 / ~215-260 ms). */
#define CH_HOLD_STOP_MS 550
#define CCT_HOLD_STOP_MS 250

#define CT_WARM 2700
#define CT_COOL 6500

typedef struct {
    ledch_t *s;
    int i;                 // channel 0..3
    char name[24];
    uint8_t target, last_on, min, max;
    uint32_t ramp;         // RampTime [ms]
    bool have_target;
    light_timer_t timed;   // Switch* with Time > 0
    uint8_t revert_to;
    uint32_t revert_ramp;
    light_hold_t hold;
    int8_t hold_dir;       // hold running on the module: +1 / -1, 0 = none
    light_timer_t hold_stop;
} chan_t;

typedef struct {
    ledch_t *s;
    int i;                 // 0..1: channels 2i (warm), 2i+1 (cold)
    char name[24];
    double v, p, last_on;  // Value 0..255, warm share 0..1
    double min, max;
    uint32_t ramp;
    bool have_target;
    bool from_module;      // after a hold: Value / p follow the reported channels
    light_timer_t timed;
    double revert_v;
    uint32_t revert_ramp;
    light_hold_t hold_v, hold_ct;
    int8_t hold_kind, hold_dir; // hold running: kind 1 Value / 2 ColorTemp, dir +1 / -1
    light_timer_t hold_stop;
} cct_t;

struct ledch {
    chan_t ch[4];
    cct_t cct[2];
    uint8_t sent[4];       // last value sent per channel
    int16_t rep[4];        // last value reported per channel, -1 = none yet
    uint8_t changed;       // channels changed by the status being parsed (bit per channel)
};

ledch_t *ledch_new(void)
{
    ledch_t *ledch_state = calloc(1, sizeof *ledch_state);
    if (!ledch_state) {
        abort(); // out of memory while loading OM.LUA
    }
    for (int i = 0; i < 4; i++) {
        ledch_state->ch[i] = (chan_t){.s = ledch_state, .i = i, .max = 255};
        ledch_state->rep[i] = -1;
    }
    for (int i = 0; i < 2; i++) {
        ledch_state->cct[i] = (cct_t){.s = ledch_state, .i = i, .p = 0.5, .max = 255};
    }
    return ledch_state;
}

// ---------------------------------------------------------------- wire
/** @brief Ramp in whole seconds, as 10 ms units (captured: 200 -> 0, 999 -> 100, 1500 -> 200). */
static uint32_t ramp_units(uint32_t ms)
{
    return ((ms > LIGHT_RAMP_MAX ? LIGHT_RAMP_MAX : ms) + 500) / 1000 * 100;
}

/** @brief One channel: SET_RAMP_TIME (t in 10 ms) + SET_CHANNEL_VALUE in one frame. */
static void queue_out(tfbus_module_t *module, ledch_t *ledch_state, int channel_index, uint32_t ramp_duration, uint8_t channel_value)
{
    uint8_t ramp_record[5] = {(uint8_t)channel_index, (uint8_t)ramp_duration, (uint8_t)(ramp_duration >> 8), 0, 0}, value_record[2] = {(uint8_t)channel_index, channel_value};
    const tfbus_tlv_t rec[2] = {{.type = 0x06, .meta = 0x00, .len = 5, .data = ramp_record},
                                {.type = 0x05, .meta = 0x10, .len = 2, .data = value_record}};
    drv_queue_records(module, rec, 2);
    ledch_state->sent[channel_index] = channel_value;
}

/** @brief Hold start of one channel: ramp (10 ms) + 13 [ch, dir 1 up / 2 down, target]. */
static void queue_hold(tfbus_module_t *module, int channel_index, uint32_t ramp_duration, int dir, uint8_t target)
{
    uint8_t ramp_record[5] = {(uint8_t)channel_index, (uint8_t)ramp_duration, (uint8_t)(ramp_duration >> 8), 0, 0};
    uint8_t hold_record[3] = {(uint8_t)channel_index, (uint8_t)(dir > 0 ? 1 : 2), target};
    const tfbus_tlv_t rec[2] = {{.type = 0x06, .meta = 0x00, .len = 5, .data = ramp_record},
                                {.type = 0x13, .meta = 0x00, .len = 3, .data = hold_record}};
    drv_queue_records(module, rec, 2);
}

/** @brief Hold stop of @p n channels from @p ch0 on. */
static void queue_hold_stop(tfbus_module_t *module, int first_channel, int channel_count)
{
    uint8_t hold_records[2][3] = {{(uint8_t)first_channel, 0, 0}, {(uint8_t)(first_channel + 1), 0, 0}};
    const tfbus_tlv_t rec[2] = {{.type = 0x13, .meta = 0x00, .len = 3, .data = hold_records[0]},
                                {.type = 0x13, .meta = 0x00, .len = 3, .data = hold_records[1]}};
    drv_queue_records(module, rec, (size_t)channel_count);
}

/** @brief Current value of channel @p ch as the CLU sees it: reported, else last sent. */
static int cur(const ledch_t *ledch_state, int channel_index)
{
    return ledch_state->rep[channel_index] >= 0 ? ledch_state->rep[channel_index] : ledch_state->sent[channel_index];
}

static void post_events(const char *name, int change, int on, int off, double old, double new_value)
{
    if (!name[0] || old == new_value) {
        return;
    }
    clu_lua_post_event(name, change);
    if (old <= 0 && new_value > 0) {
        clu_lua_post_event(name, on);
    } else if (new_value <= 0 && old > 0) {
        clu_lua_post_event(name, off);
    }
}

// ---------------------------------------------------------------- LED_CHANNEL
static void ch_out(tfbus_module_t *module, chan_t *channel_state, uint8_t channel_value, uint32_t ramp)
{
    channel_state->target = channel_value;
    channel_state->have_target = true;
    if (channel_value > 0) {
        channel_state->last_on = channel_value;
    }
    queue_out(module, channel_state->s, channel_state->i, ramp_units(ramp), channel_value);
}

static void ch_revert(tfbus_module_t *module, void *owner)
{
    chan_t *channel_state = owner;
    ch_out(module, channel_state, channel_state->revert_to, channel_state->revert_ramp);
}

static void ch_hold_stop(tfbus_module_t *module, void *owner)
{
    chan_t *channel_state = owner;
    if (channel_state->hold_dir) {
        channel_state->hold_dir = 0;
        queue_hold_stop(module, channel_state->i, 1);
    }
}

/** @brief Hold*: start (or turn) the module's ramp, keep it going until calls stop. */
static void ch_hold(tfbus_module_t *module, chan_t *channel_state, int fixed, uint32_t sweep)
{
    int64_t now = esp_timer_get_time();
    int dir = fixed;
    if (fixed) {
        channel_state->hold.last_us = now;
    } else {
        dir = light_hold_dir(&channel_state->hold, now, cur(channel_state->s, channel_state->i), channel_state->min, channel_state->max);
    }
    if (dir != channel_state->hold_dir) {
        queue_hold(module, channel_state->i, ramp_units(sweep), dir, dir > 0 ? channel_state->max : channel_state->min);
        channel_state->hold_dir = (int8_t)dir;
    }
    light_timer_start(&channel_state->hold_stop, module, channel_state, ch_hold_stop, CH_HOLD_STOP_MS);
}

static int ch_execute(tfbus_module_t *module, chan_t *channel_state, lua_State *L, int idx, int first, int nargs)
{
    switch (idx) {
    case CX_SET: {
        double requested_value = luaL_checknumber(L, first);
        uint32_t ramp = light_ramp_arg(L, first, nargs, 1, channel_state->ramp);
        if ((requested_value < channel_state->min || requested_value > channel_state->max) && channel_state->name[0]) {
            clu_lua_post_event(channel_state->name, CE_RANGE);
        }
        light_timer_cancel(&channel_state->timed);
        ch_out(module, channel_state, (uint8_t)light_clamp(floor(requested_value), channel_state->min, channel_state->max), ramp);
        return 0;
    }
    case CX_ON:
    case CX_OFF:
    case CX_SWITCH: {
        double ms = light_arg(L, first, nargs, 0, 0);
        uint32_t ramp = light_ramp_arg(L, first, nargs, 1, channel_state->ramp);
        bool last = idx != CX_OFF && light_arg(L, first, nargs, 2, 0) == 1;
        bool on = idx == CX_ON || (idx == CX_SWITCH && channel_state->target == 0);
        uint8_t requested_value = on ? (last && channel_state->last_on ? channel_state->last_on : channel_state->max) : 0;
        light_timer_cancel(&channel_state->timed);
        if (ms > 0 && requested_value != channel_state->target) {
            channel_state->revert_to = channel_state->target;
            channel_state->revert_ramp = ramp;
            light_timer_start(&channel_state->timed, module, channel_state, ch_revert, (uint32_t)ms);
        }
        ch_out(module, channel_state, requested_value, ramp);
        return 0;
    }
    case CX_HOLD:
    case CX_UP:
    case CX_DOWN:
        light_timer_cancel(&channel_state->timed);
        ch_hold(module, channel_state, idx == CX_UP ? 1 : idx == CX_DOWN ? -1 : 0, light_ramp_arg(L, first, nargs, 0, channel_state->ramp));
        return 0;
    default:
        return -1;
    }
}

// ---------------------------------------------------------------- LED_CCT
static double ct_of(double warm_share)
{
    return (double)lround(CT_COOL - (CT_COOL - CT_WARM) * warm_share);
}

/** @brief ColorTemp, ColorTemp% and Value: stored on the object (Lua hook) or posted (bus task). */
static void cct_publish(const cct_t *cct_state, lua_State *L, int obj)
{
    const int idx[3] = {T_CT, T_PCT, T_VALUE};
    const double val[3] = {ct_of(cct_state->p), (double)lround(cct_state->p * 100), cct_state->v};
    for (int i = 0; i < 3; i++) {
        if (L) {
            tfbus_obj_store(L, obj, idx[i], val[i]);
        } else if (cct_state->name[0]) {
            clu_lua_post_number(cct_state->name, idx[i], val[i]);
        }
    }
}

static void cct_events(const cct_t *cct_state, double previous_value, double previous_warm_share)
{
    if (cct_state->name[0] && previous_warm_share != cct_state->p) {
        clu_lua_post_event(cct_state->name, TE_CT);
    }
    post_events(cct_state->name, TE_CHANGE, TE_ON, TE_OFF, previous_value, cct_state->v);
}

/**
 * @brief Drive the pair to Value @p v / warm share @p p.
 * @param by_value The change is one of Value (ramp scaled by it), else of the colour temperature.
 */
static void cct_out(tfbus_module_t *module, cct_t *cct_state, lua_State *L, int obj, double output_value, double warm_share, uint32_t ramp,
                    bool by_value)
{
    const float ramp_scale = (float)ramp_units(ramp);
    const float normalized_change = by_value ? fabsf((float)output_value / 255.0f - (float)cct_state->v / 255.0f) : fabsf((float)warm_share - (float)cct_state->p);
    const int warm = (int)lround(output_value * warm_share);
    const int val[2] = {warm, (int)output_value - warm};
    for (int k = 0; k < 2; k++) {
        int channel_index = 2 * cct_state->i + k, dch = abs(val[k] - cur(cct_state->s, channel_index));
        float channel_ramp = ramp_scale * normalized_change * 255.0f / (float)(dch ? dch : 255);
        float ramp_duration = floorf(by_value ? channel_ramp - 1e-3f : channel_ramp);
        queue_out(module, cct_state->s, channel_index, (uint32_t)light_clamp(ramp_duration, 0, 0xffff), (uint8_t)val[k]);
    }
    double previous_value = cct_state->v, previous_warm_share = cct_state->p;
    cct_state->v = output_value;
    cct_state->p = warm_share;
    cct_state->have_target = true;
    cct_state->from_module = false;
    if (output_value > 0) {
        cct_state->last_on = output_value;
    }
    cct_publish(cct_state, L, obj);
    cct_events(cct_state, previous_value, previous_warm_share);
}

static void cct_revert(tfbus_module_t *module, void *owner)
{
    cct_t *cct_state = owner;
    cct_out(module, cct_state, NULL, 0, cct_state->revert_v, cct_state->p, cct_state->revert_ramp, true);
}

static void cct_hold_stop(tfbus_module_t *module, void *owner)
{
    cct_t *cct_state = owner;
    if (cct_state->hold_kind) {
        cct_state->hold_kind = cct_state->hold_dir = 0;
        queue_hold_stop(module, 2 * cct_state->i, 2);
    }
}

/** @brief Hold* of LED_CCT: @p kind 1 Value, 2 ColorTemp (+1 = warmer); @p fixed direction or 0. */
static void cct_hold(tfbus_module_t *module, cct_t *cct_state, int kind, int fixed, uint32_t sweep)
{
    int64_t now = esp_timer_get_time();
    light_hold_t *hold_state = kind == 1 ? &cct_state->hold_v : &cct_state->hold_ct;
    int dir = fixed;
    if (fixed) {
        hold_state->last_us = now;
    } else if (kind == 1) {
        dir = light_hold_dir(hold_state, now, cct_state->v, cct_state->min, cct_state->max);
    } else {
        dir = light_hold_dir(hold_state, now, cct_state->p, 0, 1);
    }
    if (kind != cct_state->hold_kind || dir != cct_state->hold_dir) {
        int warm_target, cold_target, warm_direction, cold_direction; // targets / directions of the warm and cold channel
        if (kind == 1) {
            warm_target = dir > 0 ? (int)lround(cct_state->max * cct_state->p) : 0;
            cold_target = dir > 0 ? (int)cct_state->max - warm_target : 0;
            warm_direction = cold_direction = dir;
        } else {
            warm_target = dir > 0 ? (int)cct_state->v : 0;
            cold_target = dir > 0 ? 0 : (int)cct_state->v;
            warm_direction = dir;
            cold_direction = -dir;
        }
        uint32_t ramp_duration = ramp_units(sweep);
        queue_hold(module, 2 * cct_state->i, ramp_duration, warm_direction, (uint8_t)warm_target);
        queue_hold(module, 2 * cct_state->i + 1, ramp_duration, cold_direction, (uint8_t)cold_target);
        cct_state->hold_kind = (int8_t)kind;
        cct_state->hold_dir = (int8_t)dir;
    }
    cct_state->from_module = true;
    light_timer_start(&cct_state->hold_stop, module, cct_state, cct_hold_stop, CCT_HOLD_STOP_MS);
}

static int cct_execute(tfbus_module_t *module, cct_t *cct_state, lua_State *L, int obj, int idx, int first, int nargs)
{
    switch (idx) {
    case TX_CT:
    case TX_PCT: {
        double requested_temperature = luaL_checknumber(L, first);
        double warm_share = idx == TX_CT ? (CT_COOL - light_clamp(requested_temperature, CT_WARM, CT_COOL)) / (CT_COOL - CT_WARM)
                                : light_clamp(requested_temperature, 0, 100) / 100;
        light_timer_cancel(&cct_state->timed);
        cct_out(module, cct_state, L, obj, cct_state->v, warm_share, light_ramp_arg(L, first, nargs, 1, cct_state->ramp), false);
        return 0;
    }
    case TX_VALUE: {
        double requested_value = luaL_checknumber(L, first);
        if ((requested_value < cct_state->min || requested_value > cct_state->max) && cct_state->name[0]) {
            clu_lua_post_event(cct_state->name, TE_RANGE);
        }
        light_timer_cancel(&cct_state->timed);
        cct_out(module, cct_state, L, obj, floor(light_clamp(requested_value, cct_state->min, cct_state->max)), cct_state->p,
                light_ramp_arg(L, first, nargs, 1, cct_state->ramp), true);
        return 0;
    }
    case TX_ON:
    case TX_OFF:
    case TX_SWITCH: {
        double ms = light_arg(L, first, nargs, 0, 0);
        uint32_t ramp = light_ramp_arg(L, first, nargs, 1, cct_state->ramp);
        bool last = idx != TX_OFF && light_arg(L, first, nargs, 2, 0) == 1;
        bool on = idx == TX_ON || (idx == TX_SWITCH && cct_state->v <= 0);
        double requested_value = on ? (last && cct_state->last_on > 0 ? cct_state->last_on : cct_state->max) : 0;
        light_timer_cancel(&cct_state->timed);
        if (ms > 0 && requested_value != cct_state->v) {
            cct_state->revert_v = cct_state->v;
            cct_state->revert_ramp = ramp;
            light_timer_start(&cct_state->timed, module, cct_state, cct_revert, (uint32_t)ms);
        }
        cct_out(module, cct_state, L, obj, requested_value, cct_state->p, ramp, true);
        return 0;
    }
    case TX_HOLD:
    case TX_UP:
    case TX_DOWN:
    case TX_HOLD_CT:
    case TX_WARM:
    case TX_COOL: {
        int kind = idx <= TX_DOWN ? 1 : 2;
        int fixed = idx == TX_UP || idx == TX_WARM ? 1 : idx == TX_DOWN || idx == TX_COOL ? -1 : 0;
        light_timer_cancel(&cct_state->timed);
        cct_hold(module, cct_state, kind, fixed, light_ramp_arg(L, first, nargs, 0, cct_state->ramp));
        return 0;
    }
    default:
        return -1;
    }
}

// ---------------------------------------------------------------- hooks
void ledch_io_new(ledch_t *ledch_state, int cls, int channel_index, const char *name)
{
    if (cls == LEDCH_CLS_CHANNEL && channel_index >= 0 && channel_index < 4) {
        strlcpy(ledch_state->ch[channel_index].name, name, sizeof ledch_state->ch[channel_index].name);
    } else if (cls == LEDCH_CLS_CCT && channel_index >= 0 && channel_index < 2) {
        strlcpy(ledch_state->cct[channel_index].name, name, sizeof ledch_state->cct[channel_index].name);
    }
}

bool ledch_io_set(tfbus_module_t *module, ledch_t *ledch_state, lua_State *L, int cls, int channel_index, int idx, int val)
{
    if (cls == LEDCH_CLS_CHANNEL && channel_index >= 0 && channel_index < 4) {
        chan_t *channel_state = &ledch_state->ch[channel_index];
        switch (idx) {
        case C_RAMP:
            channel_state->ramp = (uint32_t)light_clamp(luaL_checknumber(L, val), 0, LIGHT_RAMP_MAX);
            break;
        case C_MAX:
            channel_state->max = (uint8_t)light_clamp(luaL_checknumber(L, val), 0, 255);
            break;
        case C_MIN:
            channel_state->min = (uint8_t)light_clamp(luaL_checknumber(L, val), 0, 255);
            break;
        case C_DLG:
            light_queue_dlg(module, (uint8_t)channel_index, (uint32_t)light_clamp(luaL_checknumber(L, val), 0, 10000));
            break;
        default:
            break;
        }
    } else if (cls == LEDCH_CLS_CCT && channel_index >= 0 && channel_index < 2) {
        cct_t *channel_state = &ledch_state->cct[channel_index];
        switch (idx) {
        case T_RAMP:
            channel_state->ramp = (uint32_t)light_clamp(luaL_checknumber(L, val), 0, LIGHT_RAMP_MAX);
            break;
        case T_MAX:
            channel_state->max = light_clamp(luaL_checknumber(L, val), 0, 255);
            break;
        case T_MIN:
            channel_state->min = light_clamp(luaL_checknumber(L, val), 0, 255);
            break;
        default:
            break;
        }
    }
    return false; // store generically
}

int ledch_io_execute(tfbus_module_t *module, ledch_t *ledch_state, lua_State *L, int obj, int cls, int channel_index, int idx,
                     int first, int nargs)
{
    if (cls == LEDCH_CLS_CHANNEL && channel_index >= 0 && channel_index < 4) {
        return ch_execute(module, &ledch_state->ch[channel_index], L, idx, first, nargs);
    }
    if (cls == LEDCH_CLS_CCT && channel_index >= 0 && channel_index < 2) {
        return cct_execute(module, &ledch_state->cct[channel_index], L, obj, idx, first, nargs);
    }
    return -1;
}

void ledch_reported(ledch_t *ledch_state, int channel_index, uint8_t reported_value)
{
    if (channel_index < 0 || channel_index > 3) {
        return;
    }
    int old = ledch_state->rep[channel_index];
    ledch_state->rep[channel_index] = reported_value;
    if (old != reported_value) {
        ledch_state->changed |= (uint8_t)(1u << channel_index);
    }
    const chan_t *channel_state = &ledch_state->ch[channel_index];
    if (channel_state->name[0] && old != reported_value) {
        clu_lua_post_number(channel_state->name, C_VALUE, reported_value);
        if (old >= 0) {
            post_events(channel_state->name, CE_CHANGE, CE_ON, CE_OFF, old, reported_value);
            clu_lua_post_event(channel_state->name, reported_value > old ? CE_RISE : CE_LOWER);
        }
    }
}

void ledch_reported_end(ledch_t *ledch_state)
{
    for (int i = 0; i < 2; i++) {
        cct_t *cct_state = &ledch_state->cct[i];
        int warm_value = ledch_state->rep[2 * i], cold_value = ledch_state->rep[2 * i + 1];
        if (!cct_state->name[0] || !(ledch_state->changed >> (2 * i) & 3)) {
            continue;
        }
        if (ledch_state->changed >> (2 * i) & 1) {
            clu_lua_post_number(cct_state->name, T_WARM, warm_value);
        }
        if (ledch_state->changed >> (2 * i + 1) & 1) {
            clu_lua_post_number(cct_state->name, T_COLD, cold_value);
        }
        if (cct_state->from_module && warm_value >= 0 && cold_value >= 0) { // after a hold: Value / colour temperature from the module
            double previous_value = cct_state->v, previous_warm_share = cct_state->p;
            cct_state->v = warm_value + cold_value;
            if (cct_state->v > 0) {
                cct_state->p = (double)warm_value / cct_state->v;
                cct_state->last_on = cct_state->v;
            }
            cct_publish(cct_state, NULL, 0);
            cct_events(cct_state, previous_value, previous_warm_share);
        }
    }
    ledch_state->changed = 0;
}

void ledch_init(tfbus_module_t *module, ledch_t *ledch_state)
{
    for (int i = 0; i < 4; i++) {
        if (ledch_state->ch[i].have_target) {
            queue_out(module, ledch_state, i, 0, ledch_state->ch[i].target);
        }
    }
    for (int i = 0; i < 2; i++) {
        const cct_t *cct_state = &ledch_state->cct[i];
        if (cct_state->have_target) {
            int warm = (int)lround(cct_state->v * cct_state->p);
            queue_out(module, ledch_state, 2 * i, 0, (uint8_t)warm);
            queue_out(module, ledch_state, 2 * i + 1, 0, (uint8_t)(cct_state->v - warm));
        }
    }
}

void ledch_overdue(ledch_t *ledch_state)
{
    for (int i = 0; i < 4; i++) {
        light_timer_overdue(&ledch_state->ch[i].timed);
        light_timer_overdue(&ledch_state->ch[i].hold_stop);
    }
    for (int i = 0; i < 2; i++) {
        light_timer_overdue(&ledch_state->cct[i].timed);
        light_timer_overdue(&ledch_state->cct[i].hold_stop);
    }
}

void ledch_deinit(ledch_t *ledch_state)
{
    for (int i = 0; i < 4; i++) {
        light_timer_delete(&ledch_state->ch[i].timed);
        light_timer_delete(&ledch_state->ch[i].hold_stop);
    }
    for (int i = 0; i < 2; i++) {
        light_timer_delete(&ledch_state->cct[i].timed);
        light_timer_delete(&ledch_state->cct[i].hold_stop);
    }
}
