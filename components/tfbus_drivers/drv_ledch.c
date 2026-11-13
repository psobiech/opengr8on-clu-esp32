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
    ledch_t *s = calloc(1, sizeof *s);
    if (!s) {
        abort(); // out of memory while loading OM.LUA
    }
    for (int i = 0; i < 4; i++) {
        s->ch[i] = (chan_t){.s = s, .i = i, .max = 255};
        s->rep[i] = -1;
    }
    for (int i = 0; i < 2; i++) {
        s->cct[i] = (cct_t){.s = s, .i = i, .p = 0.5, .max = 255};
    }
    return s;
}

// ---------------------------------------------------------------- wire
/** @brief Ramp in whole seconds, as 10 ms units (captured: 200 -> 0, 999 -> 100, 1500 -> 200). */
static uint32_t ramp_units(uint32_t ms)
{
    return ((ms > LIGHT_RAMP_MAX ? LIGHT_RAMP_MAX : ms) + 500) / 1000 * 100;
}

/** @brief One channel: SET_RAMP_TIME (t in 10 ms) + SET_CHANNEL_VALUE in one frame. */
static void queue_out(tfbus_module_t *m, ledch_t *s, int ch, uint32_t t, uint8_t v)
{
    uint8_t r[5] = {(uint8_t)ch, (uint8_t)t, (uint8_t)(t >> 8), 0, 0}, d[2] = {(uint8_t)ch, v};
    const tfbus_tlv_t rec[2] = {{.type = 0x06, .meta = 0x00, .len = 5, .data = r},
                                {.type = 0x05, .meta = 0x10, .len = 2, .data = d}};
    drv_queue_records(m, rec, 2);
    s->sent[ch] = v;
}

/** @brief Hold start of one channel: ramp (10 ms) + 13 [ch, dir 1 up / 2 down, target]. */
static void queue_hold(tfbus_module_t *m, int ch, uint32_t t, int dir, uint8_t target)
{
    uint8_t r[5] = {(uint8_t)ch, (uint8_t)t, (uint8_t)(t >> 8), 0, 0};
    uint8_t h[3] = {(uint8_t)ch, (uint8_t)(dir > 0 ? 1 : 2), target};
    const tfbus_tlv_t rec[2] = {{.type = 0x06, .meta = 0x00, .len = 5, .data = r},
                                {.type = 0x13, .meta = 0x00, .len = 3, .data = h}};
    drv_queue_records(m, rec, 2);
}

/** @brief Hold stop of @p n channels from @p ch0 on. */
static void queue_hold_stop(tfbus_module_t *m, int ch0, int n)
{
    uint8_t h[2][3] = {{(uint8_t)ch0, 0, 0}, {(uint8_t)(ch0 + 1), 0, 0}};
    const tfbus_tlv_t rec[2] = {{.type = 0x13, .meta = 0x00, .len = 3, .data = h[0]},
                                {.type = 0x13, .meta = 0x00, .len = 3, .data = h[1]}};
    drv_queue_records(m, rec, (size_t)n);
}

/** @brief Current value of channel @p ch as the CLU sees it: reported, else last sent. */
static int cur(const ledch_t *s, int ch)
{
    return s->rep[ch] >= 0 ? s->rep[ch] : s->sent[ch];
}

static void post_events(const char *name, int change, int on, int off, double old, double nv)
{
    if (!name[0] || old == nv) {
        return;
    }
    clu_lua_post_event(name, change);
    if (old <= 0 && nv > 0) {
        clu_lua_post_event(name, on);
    } else if (nv <= 0 && old > 0) {
        clu_lua_post_event(name, off);
    }
}

// ---------------------------------------------------------------- LED_CHANNEL
static void ch_out(tfbus_module_t *m, chan_t *c, uint8_t v, uint32_t ramp)
{
    c->target = v;
    c->have_target = true;
    if (v > 0) {
        c->last_on = v;
    }
    queue_out(m, c->s, c->i, ramp_units(ramp), v);
}

static void ch_revert(tfbus_module_t *m, void *owner)
{
    chan_t *c = owner;
    ch_out(m, c, c->revert_to, c->revert_ramp);
}

static void ch_hold_stop(tfbus_module_t *m, void *owner)
{
    chan_t *c = owner;
    if (c->hold_dir) {
        c->hold_dir = 0;
        queue_hold_stop(m, c->i, 1);
    }
}

/** @brief Hold*: start (or turn) the module's ramp, keep it going until calls stop. */
static void ch_hold(tfbus_module_t *m, chan_t *c, int fixed, uint32_t sweep)
{
    int64_t now = esp_timer_get_time();
    int dir = fixed;
    if (fixed) {
        c->hold.last_us = now;
    } else {
        dir = light_hold_dir(&c->hold, now, cur(c->s, c->i), c->min, c->max);
    }
    if (dir != c->hold_dir) {
        queue_hold(m, c->i, ramp_units(sweep), dir, dir > 0 ? c->max : c->min);
        c->hold_dir = (int8_t)dir;
    }
    light_timer_start(&c->hold_stop, m, c, ch_hold_stop, CH_HOLD_STOP_MS);
}

static int ch_execute(tfbus_module_t *m, chan_t *c, lua_State *L, int idx, int first, int nargs)
{
    switch (idx) {
    case CX_SET: {
        double v = luaL_checknumber(L, first);
        uint32_t ramp = light_ramp_arg(L, first, nargs, 1, c->ramp);
        if ((v < c->min || v > c->max) && c->name[0]) {
            clu_lua_post_event(c->name, CE_RANGE);
        }
        light_timer_cancel(&c->timed);
        ch_out(m, c, (uint8_t)light_clamp(floor(v), c->min, c->max), ramp);
        return 0;
    }
    case CX_ON:
    case CX_OFF:
    case CX_SWITCH: {
        double ms = light_arg(L, first, nargs, 0, 0);
        uint32_t ramp = light_ramp_arg(L, first, nargs, 1, c->ramp);
        bool last = idx != CX_OFF && light_arg(L, first, nargs, 2, 0) == 1;
        bool on = idx == CX_ON || (idx == CX_SWITCH && c->target == 0);
        uint8_t v = on ? (last && c->last_on ? c->last_on : c->max) : 0;
        light_timer_cancel(&c->timed);
        if (ms > 0 && v != c->target) {
            c->revert_to = c->target;
            c->revert_ramp = ramp;
            light_timer_start(&c->timed, m, c, ch_revert, (uint32_t)ms);
        }
        ch_out(m, c, v, ramp);
        return 0;
    }
    case CX_HOLD:
    case CX_UP:
    case CX_DOWN:
        light_timer_cancel(&c->timed);
        ch_hold(m, c, idx == CX_UP ? 1 : idx == CX_DOWN ? -1 : 0, light_ramp_arg(L, first, nargs, 0, c->ramp));
        return 0;
    default:
        return -1;
    }
}

// ---------------------------------------------------------------- LED_CCT
static double ct_of(double p)
{
    return (double)lround(CT_COOL - (CT_COOL - CT_WARM) * p);
}

/** @brief ColorTemp, ColorTemp% and Value: stored on the object (Lua hook) or posted (bus task). */
static void cct_publish(const cct_t *c, lua_State *L, int obj)
{
    const int idx[3] = {T_CT, T_PCT, T_VALUE};
    const double val[3] = {ct_of(c->p), (double)lround(c->p * 100), c->v};
    for (int i = 0; i < 3; i++) {
        if (L) {
            tfbus_obj_store(L, obj, idx[i], val[i]);
        } else if (c->name[0]) {
            clu_lua_post_number(c->name, idx[i], val[i]);
        }
    }
}

static void cct_events(const cct_t *c, double v0, double p0)
{
    if (c->name[0] && p0 != c->p) {
        clu_lua_post_event(c->name, TE_CT);
    }
    post_events(c->name, TE_CHANGE, TE_ON, TE_OFF, v0, c->v);
}

/**
 * @brief Drive the pair to Value @p v / warm share @p p.
 * @param by_value The change is one of Value (ramp scaled by it), else of the colour temperature.
 */
static void cct_out(tfbus_module_t *m, cct_t *c, lua_State *L, int obj, double v, double p, uint32_t ramp,
                    bool by_value)
{
    const float r = (float)ramp_units(ramp);
    const float d = by_value ? fabsf((float)v / 255.0f - (float)c->v / 255.0f) : fabsf((float)p - (float)c->p);
    const int warm = (int)lround(v * p);
    const int val[2] = {warm, (int)v - warm};
    for (int k = 0; k < 2; k++) {
        int ch = 2 * c->i + k, dch = abs(val[k] - cur(c->s, ch));
        float x = r * d * 255.0f / (float)(dch ? dch : 255);
        float t = floorf(by_value ? x - 1e-3f : x);
        queue_out(m, c->s, ch, (uint32_t)light_clamp(t, 0, 0xffff), (uint8_t)val[k]);
    }
    double v0 = c->v, p0 = c->p;
    c->v = v;
    c->p = p;
    c->have_target = true;
    c->from_module = false;
    if (v > 0) {
        c->last_on = v;
    }
    cct_publish(c, L, obj);
    cct_events(c, v0, p0);
}

static void cct_revert(tfbus_module_t *m, void *owner)
{
    cct_t *c = owner;
    cct_out(m, c, NULL, 0, c->revert_v, c->p, c->revert_ramp, true);
}

static void cct_hold_stop(tfbus_module_t *m, void *owner)
{
    cct_t *c = owner;
    if (c->hold_kind) {
        c->hold_kind = c->hold_dir = 0;
        queue_hold_stop(m, 2 * c->i, 2);
    }
}

/** @brief Hold* of LED_CCT: @p kind 1 Value, 2 ColorTemp (+1 = warmer); @p fixed direction or 0. */
static void cct_hold(tfbus_module_t *m, cct_t *c, int kind, int fixed, uint32_t sweep)
{
    int64_t now = esp_timer_get_time();
    light_hold_t *h = kind == 1 ? &c->hold_v : &c->hold_ct;
    int dir = fixed;
    if (fixed) {
        h->last_us = now;
    } else if (kind == 1) {
        dir = light_hold_dir(h, now, c->v, c->min, c->max);
    } else {
        dir = light_hold_dir(h, now, c->p, 0, 1);
    }
    if (kind != c->hold_kind || dir != c->hold_dir) {
        int tw, tc, dw, dc; // targets / directions of the warm and cold channel
        if (kind == 1) {
            tw = dir > 0 ? (int)lround(c->max * c->p) : 0;
            tc = dir > 0 ? (int)c->max - tw : 0;
            dw = dc = dir;
        } else {
            tw = dir > 0 ? (int)c->v : 0;
            tc = dir > 0 ? 0 : (int)c->v;
            dw = dir;
            dc = -dir;
        }
        uint32_t t = ramp_units(sweep);
        queue_hold(m, 2 * c->i, t, dw, (uint8_t)tw);
        queue_hold(m, 2 * c->i + 1, t, dc, (uint8_t)tc);
        c->hold_kind = (int8_t)kind;
        c->hold_dir = (int8_t)dir;
    }
    c->from_module = true;
    light_timer_start(&c->hold_stop, m, c, cct_hold_stop, CCT_HOLD_STOP_MS);
}

static int cct_execute(tfbus_module_t *m, cct_t *c, lua_State *L, int obj, int idx, int first, int nargs)
{
    switch (idx) {
    case TX_CT:
    case TX_PCT: {
        double a = luaL_checknumber(L, first);
        double p = idx == TX_CT ? (CT_COOL - light_clamp(a, CT_WARM, CT_COOL)) / (CT_COOL - CT_WARM)
                                : light_clamp(a, 0, 100) / 100;
        light_timer_cancel(&c->timed);
        cct_out(m, c, L, obj, c->v, p, light_ramp_arg(L, first, nargs, 1, c->ramp), false);
        return 0;
    }
    case TX_VALUE: {
        double v = luaL_checknumber(L, first);
        if ((v < c->min || v > c->max) && c->name[0]) {
            clu_lua_post_event(c->name, TE_RANGE);
        }
        light_timer_cancel(&c->timed);
        cct_out(m, c, L, obj, floor(light_clamp(v, c->min, c->max)), c->p,
                light_ramp_arg(L, first, nargs, 1, c->ramp), true);
        return 0;
    }
    case TX_ON:
    case TX_OFF:
    case TX_SWITCH: {
        double ms = light_arg(L, first, nargs, 0, 0);
        uint32_t ramp = light_ramp_arg(L, first, nargs, 1, c->ramp);
        bool last = idx != TX_OFF && light_arg(L, first, nargs, 2, 0) == 1;
        bool on = idx == TX_ON || (idx == TX_SWITCH && c->v <= 0);
        double v = on ? (last && c->last_on > 0 ? c->last_on : c->max) : 0;
        light_timer_cancel(&c->timed);
        if (ms > 0 && v != c->v) {
            c->revert_v = c->v;
            c->revert_ramp = ramp;
            light_timer_start(&c->timed, m, c, cct_revert, (uint32_t)ms);
        }
        cct_out(m, c, L, obj, v, c->p, ramp, true);
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
        light_timer_cancel(&c->timed);
        cct_hold(m, c, kind, fixed, light_ramp_arg(L, first, nargs, 0, c->ramp));
        return 0;
    }
    default:
        return -1;
    }
}

// ---------------------------------------------------------------- hooks
void ledch_io_new(ledch_t *s, int cls, int ch, const char *name)
{
    if (cls == LEDCH_CLS_CHANNEL && ch >= 0 && ch < 4) {
        strlcpy(s->ch[ch].name, name, sizeof s->ch[ch].name);
    } else if (cls == LEDCH_CLS_CCT && ch >= 0 && ch < 2) {
        strlcpy(s->cct[ch].name, name, sizeof s->cct[ch].name);
    }
}

bool ledch_io_set(tfbus_module_t *m, ledch_t *s, lua_State *L, int cls, int ch, int idx, int val)
{
    if (cls == LEDCH_CLS_CHANNEL && ch >= 0 && ch < 4) {
        chan_t *c = &s->ch[ch];
        switch (idx) {
        case C_RAMP:
            c->ramp = (uint32_t)light_clamp(luaL_checknumber(L, val), 0, LIGHT_RAMP_MAX);
            break;
        case C_MAX:
            c->max = (uint8_t)light_clamp(luaL_checknumber(L, val), 0, 255);
            break;
        case C_MIN:
            c->min = (uint8_t)light_clamp(luaL_checknumber(L, val), 0, 255);
            break;
        case C_DLG:
            light_queue_dlg(m, (uint8_t)ch, (uint32_t)light_clamp(luaL_checknumber(L, val), 0, 10000));
            break;
        default:
            break;
        }
    } else if (cls == LEDCH_CLS_CCT && ch >= 0 && ch < 2) {
        cct_t *c = &s->cct[ch];
        switch (idx) {
        case T_RAMP:
            c->ramp = (uint32_t)light_clamp(luaL_checknumber(L, val), 0, LIGHT_RAMP_MAX);
            break;
        case T_MAX:
            c->max = light_clamp(luaL_checknumber(L, val), 0, 255);
            break;
        case T_MIN:
            c->min = light_clamp(luaL_checknumber(L, val), 0, 255);
            break;
        default:
            break;
        }
    }
    return false; // store generically
}

int ledch_io_execute(tfbus_module_t *m, ledch_t *s, lua_State *L, int obj, int cls, int ch, int idx,
                     int first, int nargs)
{
    if (cls == LEDCH_CLS_CHANNEL && ch >= 0 && ch < 4) {
        return ch_execute(m, &s->ch[ch], L, idx, first, nargs);
    }
    if (cls == LEDCH_CLS_CCT && ch >= 0 && ch < 2) {
        return cct_execute(m, &s->cct[ch], L, obj, idx, first, nargs);
    }
    return -1;
}

void ledch_reported(ledch_t *s, int ch, uint8_t v)
{
    if (ch < 0 || ch > 3) {
        return;
    }
    int old = s->rep[ch];
    s->rep[ch] = v;
    if (old != v) {
        s->changed |= (uint8_t)(1u << ch);
    }
    const chan_t *c = &s->ch[ch];
    if (c->name[0] && old != v) {
        clu_lua_post_number(c->name, C_VALUE, v);
        if (old >= 0) {
            post_events(c->name, CE_CHANGE, CE_ON, CE_OFF, old, v);
            clu_lua_post_event(c->name, v > old ? CE_RISE : CE_LOWER);
        }
    }
}

void ledch_reported_end(ledch_t *s)
{
    for (int i = 0; i < 2; i++) {
        cct_t *t = &s->cct[i];
        int w = s->rep[2 * i], k = s->rep[2 * i + 1];
        if (!t->name[0] || !(s->changed >> (2 * i) & 3)) {
            continue;
        }
        if (s->changed >> (2 * i) & 1) {
            clu_lua_post_number(t->name, T_WARM, w);
        }
        if (s->changed >> (2 * i + 1) & 1) {
            clu_lua_post_number(t->name, T_COLD, k);
        }
        if (t->from_module && w >= 0 && k >= 0) { // after a hold: Value / colour temperature from the module
            double v0 = t->v, p0 = t->p;
            t->v = w + k;
            if (t->v > 0) {
                t->p = (double)w / t->v;
                t->last_on = t->v;
            }
            cct_publish(t, NULL, 0);
            cct_events(t, v0, p0);
        }
    }
    s->changed = 0;
}

void ledch_init(tfbus_module_t *m, ledch_t *s)
{
    for (int i = 0; i < 4; i++) {
        if (s->ch[i].have_target) {
            queue_out(m, s, i, 0, s->ch[i].target);
        }
    }
    for (int i = 0; i < 2; i++) {
        const cct_t *c = &s->cct[i];
        if (c->have_target) {
            int warm = (int)lround(c->v * c->p);
            queue_out(m, s, 2 * i, 0, (uint8_t)warm);
            queue_out(m, s, 2 * i + 1, 0, (uint8_t)(c->v - warm));
        }
    }
}

void ledch_overdue(ledch_t *s)
{
    for (int i = 0; i < 4; i++) {
        light_timer_overdue(&s->ch[i].timed);
        light_timer_overdue(&s->ch[i].hold_stop);
    }
    for (int i = 0; i < 2; i++) {
        light_timer_overdue(&s->cct[i].timed);
        light_timer_overdue(&s->cct[i].hold_stop);
    }
}

void ledch_deinit(ledch_t *s)
{
    for (int i = 0; i < 4; i++) {
        light_timer_delete(&s->ch[i].timed);
        light_timer_delete(&s->ch[i].hold_stop);
    }
    for (int i = 0; i < 2; i++) {
        light_timer_delete(&s->cct[i].timed);
        light_timer_delete(&s->cct[i].hold_stop);
    }
}
