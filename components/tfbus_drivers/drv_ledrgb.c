/**
 * @file drv_ledrgb.c
 * @brief LED_RGBW_DIN (hwType 24 / 0x18): 1x LEDRGB (class 9), 1x PowerSupplyVoltage (class 12).
 *
 * Interface module_2_0_LED_RGBW_DIN_fv04_02.xml. Wire format from captures of a real CLU
 * (clu firmware/tfbus_capture_20261014_180929.log, CLU_2 addr 5; docs/TFBUS_LED_RGBW_DIN.md):
 * @verbatim
 *   output : SET_RAMP_TIME[0, ms LE u16, 0, 0]  then  one frame of 4 records
 *            SET_CHANNEL_VALUE 0x05 meta 0x10 [ch, value 0-255]  (ch 0 R, 1 G, 2 B, 3 W)
 *   status : 01 | 02 02 10 [U LE u16, 0.01 V]   (~24 V on CLU_2: PowerSupplyVoltage)
 * @endverbatim
 * The real CLU sends nothing to these modules at startup (refresh capture).
 *
 * The colour model lives in the CLU (semantics from the interface, not checked against a real CLU):
 * - State is HSV (Hue, Saturation, Value) + R/G/B (derived) + W (independent). Setting R/G/B or
 *   the RGB string recomputes HSV; hue / saturation are kept while Value is 0 or the colour is grey.
 * - RedCorrection / GreenCorrection / BlueCorrection [%] scale the R/G/B sent to the module only.
 * - SetValue clamps to [MinValue, MaxValue] (OnOutOfRange); SwitchOn -> MaxValue with the last W,
 *   SwitchOff -> all channels 0; Time > 0 restores the previous colour after Time ms.
 * - Hold* step on each call (see light_hold_step()); Ramp = full sweep time; HoldHue wraps.
 * - Events: OnValueChange on any channel change; OnValueRise / OnValueLower by Value;
 *   OnSwitchOn / OnSwitchOff when the output goes from / to all channels 0.
 * - StatisticState / Load: stored only.
 * PowerSupplyVoltage: Value, Value% (of MaxValue), events by Sensitivity, OnOutOfRange / OnInRange.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "drv_light.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include "tfbus_drivers.h"
#include "tfbus_master.h"

enum { CLS_LEDRGB = 9, CLS_PSU = 12 };

/** @brief LEDRGB feature indices. */
enum { F_VALUE = 0, F_HUE = 1, F_SAT = 2, F_RED = 3, F_GREEN = 4, F_BLUE = 5, F_RGB = 6, F_RAMP = 7,
       F_MAX = 8, F_MIN = 9, F_RCORR = 10, F_GCORR = 11, F_BCORR = 12, F_WHITE = 15 };
/** @brief LEDRGB execute methods. */
enum { X_VALUE = 0, X_HUE = 1, X_SAT = 2, X_RED = 3, X_GREEN = 4, X_BLUE = 5, X_RGB = 6,
       X_HOLD = 7, X_HOLD_HUE = 8, X_ON = 9, X_OFF = 10, X_SWITCH = 11, X_WHITE = 12,
       X_HOLD_RED = 13, X_HOLD_WHITE = 16 };
/** @brief LEDRGB events. */
enum { EV_CHANGE = 0, EV_ON = 1, EV_OFF = 2, EV_RISE = 3, EV_LOWER = 4, EV_RANGE = 5 };

/** @brief PowerSupplyVoltage features / events. */
enum { P_VALUE = 0, P_PERCENT = 1, P_SENS = 3, P_MIN = 5, P_MAX = 6 };
enum { PE_CHANGE = 0, PE_LOWER = 1, PE_RISE = 2, PE_OUT = 3, PE_IN = 6 };

/** @brief Colour state. */
typedef struct {
    double h, s, v;        // hue 0..360, saturation / value 0..1
    uint8_t c[4];          // R, G, B, W 0..255 (before correction)
} color_t;

/** @brief Driver state per module (drv_ctx). */
typedef struct {
    char name[24];         // LEDRGB object (empty = not declared)
    char psu[24];          // PowerSupplyVoltage object
    color_t col;
    uint8_t w_on;          // W restored by SwitchOn
    double min, max;
    uint8_t corr[3];       // R/G/B correction [%]
    uint32_t ramp;         // RampTime [ms]
    bool have_target;      // Lua set a colour (re-applied by init)
    light_timer_t timed;   // Switch* with Time > 0
    color_t revert_to;
    uint32_t revert_ramp;
    light_hold_t hold[6];  // HoldValue, HoldHue, HoldRed/Green/Blue/White

    // PowerSupplyVoltage
    double u, u_event;     // last value, value at the last change event
    double u_sens, u_min, u_max;
    bool u_have, u_out;
} rgbw_t;

/** @brief Driver state of @p m, allocated on first use. */
static rgbw_t *ctx(tfbus_module_t *m)
{
    if (!m->drv_ctx) {
        rgbw_t *c = calloc(1, sizeof *c);
        if (!c) {
            abort(); // out of memory while loading OM.LUA
        }
        c->max = 1;
        c->corr[0] = c->corr[1] = c->corr[2] = 100;
        c->u_sens = 0.5;
        c->u_min = 21;
        c->u_max = 24;
        m->drv_ctx = c;
    }
    return m->drv_ctx;
}

// ---------------------------------------------------------------- colour model
/** @brief R/G/B from H/S/V (W untouched). */
static void hsv_to_rgb(color_t *k)
{
    double h = fmod(k->h, 360) / 60, s = k->s, v = k->v;
    int i = (int)h;
    double f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    double rgb[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
    for (int j = 0; j < 3; j++) {
        k->c[j] = (uint8_t)lround(rgb[i % 6][j] * 255);
    }
}

/** @brief H/S/V from R/G/B; hue / saturation kept where undefined (black, grey). */
static void rgb_to_hsv(color_t *k)
{
    double r = k->c[0] / 255.0, g = k->c[1] / 255.0, b = k->c[2] / 255.0;
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), d = mx - mn;
    k->v = mx;
    if (mx == 0) {
        return; // black: keep hue / saturation
    }
    k->s = d / mx;
    if (d == 0) {
        return; // grey: keep hue
    }
    double h = mx == r ? fmod((g - b) / d, 6) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
    h *= 60;
    k->h = h < 0 ? h + 360 : h;
}

static bool lit(const color_t *k)
{
    return k->c[0] || k->c[1] || k->c[2] || k->c[3];
}

/** @brief "#RRGGBB" of the R/G/B components. */
static void rgb_str(const color_t *k, char out[8])
{
    snprintf(out, 8, "#%02X%02X%02X", k->c[0], k->c[1], k->c[2]);
}

// ---------------------------------------------------------------- output
/** @brief Queue SET_RAMP_TIME + SET_CHANNEL_VALUE x4 (R/G/B corrected). */
static void send_color(tfbus_module_t *m, const rgbw_t *c, uint32_t ramp)
{
    uint8_t d[4][2];
    tfbus_tlv_t r[4];
    for (int i = 0; i < 4; i++) {
        unsigned v = c->col.c[i];
        if (i < 3) {
            v = (v * c->corr[i] + 50) / 100;
        }
        d[i][0] = (uint8_t)i;
        d[i][1] = (uint8_t)v;
        r[i] = (tfbus_tlv_t){.type = 0x05, .meta = 0x10, .len = 2, .data = d[i]};
    }
    light_queue_ramp(m, ramp);
    drv_queue_records(m, r, 4);
}

/** @brief Report the colour features: stored on the object (Lua hook, L != NULL) or posted (bus task). */
static void publish(const rgbw_t *c, lua_State *L, int obj)
{
    static const int idx[] = {F_VALUE, F_HUE, F_SAT, F_RED, F_GREEN, F_BLUE, F_WHITE};
    const color_t *k = &c->col;
    double val[] = {k->v, k->h, k->s, k->c[0], k->c[1], k->c[2], k->c[3]};
    char hex[8];
    rgb_str(k, hex);
    if (L) {
        for (int i = 0; i < 7; i++) {
            tfbus_obj_store(L, obj, idx[i], val[i]);
        }
        lua_getfield(L, obj, "_f");
        lua_pushstring(L, hex);
        lua_rawseti(L, -2, F_RGB);
        lua_pop(L, 1);
    } else if (c->name[0]) {
        for (int i = 0; i < 7; i++) {
            clu_lua_post_number(c->name, idx[i], val[i]);
        }
        clu_lua_post_string(c->name, F_RGB, hex);
    }
}

/** @brief Fire the events for a colour change @p old -> current. */
static void events(const rgbw_t *c, const color_t *old)
{
    const color_t *k = &c->col;
    if (!c->name[0] || !memcmp(old->c, k->c, sizeof k->c)) {
        return;
    }
    clu_lua_post_event(c->name, EV_CHANGE);
    if (k->v != old->v) {
        clu_lua_post_event(c->name, k->v > old->v ? EV_RISE : EV_LOWER);
    }
    if (!lit(old) && lit(k)) {
        clu_lua_post_event(c->name, EV_ON);
    } else if (lit(old) && !lit(k)) {
        clu_lua_post_event(c->name, EV_OFF);
    }
}

/** @brief Drive the module to @p k and report it (see publish() for @p L / @p obj). */
static void apply(tfbus_module_t *m, rgbw_t *c, lua_State *L, int obj, const color_t *k, uint32_t ramp)
{
    color_t old = c->col;
    c->col = *k;
    c->have_target = true;
    if (lit(k)) {
        c->w_on = k->c[3]; // W that SwitchOn brings back
    }
    send_color(m, c, ramp);
    publish(c, L, obj);
    events(c, &old);
}

/** @brief Timed switch expired (bus task): restore the previous colour. */
static void revert(tfbus_module_t *m, void *owner)
{
    rgbw_t *c = owner;
    apply(m, c, NULL, 0, &c->revert_to, c->revert_ramp);
}

// ---------------------------------------------------------------- driver hooks
/** @brief tfbus_driver_t::deinit: stop the timer. */
static void deinit(tfbus_module_t *m)
{
    light_timer_delete(&((rgbw_t *)m->drv_ctx)->timed);
}

/** @brief tfbus_driver_t::io_new: remember the LEDRGB / PowerSupplyVoltage names. */
static void io_new(tfbus_module_t *m, int cls, int ch, const char *name)
{
    rgbw_t *c = ctx(m);
    if (cls == CLS_LEDRGB && ch == 0) {
        strlcpy(c->name, name, sizeof c->name);
    } else if (cls == CLS_PSU && ch == 0) {
        strlcpy(c->psu, name, sizeof c->psu);
    }
}

/** @brief tfbus_driver_t::init: re-apply the colour Lua set (module re-addressed / back online). */
static void init(tfbus_module_t *m)
{
    rgbw_t *c = ctx(m);
    if (c->have_target) {
        send_color(m, c, 0);
    }
}

/** @brief tfbus_driver_t::io_set: cache RampTime / Min / MaxValue / corrections and PSU limits. */
static bool io_set(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int val)
{
    (void)obj;
    rgbw_t *c = ctx(m);
    if (ch != 0) {
        return false;
    }
    if (cls == CLS_PSU) {
        double v = luaL_checknumber(L, val);
        if (idx == P_SENS) {
            c->u_sens = v;
        } else if (idx == P_MIN) {
            c->u_min = v;
        } else if (idx == P_MAX) {
            c->u_max = v;
        }
        return false;
    }
    if (cls != CLS_LEDRGB) {
        return false;
    }
    switch (idx) {
    case F_RAMP:
        c->ramp = (uint32_t)light_clamp(luaL_checknumber(L, val), 0, LIGHT_RAMP_MAX);
        break;
    case F_MIN:
        c->min = light_clamp(luaL_checknumber(L, val), 0, 1);
        break;
    case F_MAX:
        c->max = light_clamp(luaL_checknumber(L, val), 0, 1);
        break;
    case F_RCORR:
    case F_GCORR:
    case F_BCORR: {
        uint8_t p = (uint8_t)light_clamp(luaL_checknumber(L, val), 0, 100);
        if (p != c->corr[idx - F_RCORR]) {
            c->corr[idx - F_RCORR] = p;
            if (c->have_target) {
                send_color(m, c, 0);
            }
        }
        break;
    }
    default:
        break;
    }
    return false; // store generically
}

/** @brief Parse "#RRGGBB" / "RRGGBB" into k->c[0..2]; false if malformed. */
static bool parse_rgb(const char *s, color_t *k)
{
    if (*s == '#') {
        s++;
    }
    if (strlen(s) != 6 || strspn(s, "0123456789abcdefABCDEF") != 6) {
        return false;
    }
    unsigned long x = strtoul(s, NULL, 16);
    k->c[0] = (uint8_t)(x >> 16);
    k->c[1] = (uint8_t)(x >> 8);
    k->c[2] = (uint8_t)x;
    return true;
}

/** @brief Hold* methods: step Value / Hue / one channel; @p sweep = full-range time [ms]. */
static void hold(tfbus_module_t *m, rgbw_t *c, lua_State *L, int obj, int idx, uint32_t sweep)
{
    color_t k = c->col;
    int which = idx == X_HOLD ? 0 : idx == X_HOLD_HUE ? 1 : 2 + (idx - X_HOLD_RED);
    light_hold_t *h = &c->hold[which];
    int64_t now = esp_timer_get_time();
    uint32_t step;
    if (which == 0) {
        step = light_hold_step(h, now, k.v, c->min, c->max);
        double d = sweep ? (c->max - c->min) * step / sweep : 1;
        k.v = light_clamp(k.v + h->dir * d, c->min, c->max);
        hsv_to_rgb(&k);
    } else if (which == 1) {
        step = light_hold_step(h, now, 0, -1, 1); // hue only goes round
        h->dir = 1;
        k.h = fmod(k.h + (sweep ? 360.0 * step / sweep : 60), 360);
        hsv_to_rgb(&k);
    } else {
        int ch = which - 2; // 0 R, 1 G, 2 B, 3 W
        step = light_hold_step(h, now, k.c[ch], 0, 255);
        double d = sweep ? 255.0 * step / sweep : 255;
        k.c[ch] = (uint8_t)lround(light_clamp(k.c[ch] + h->dir * d, 0, 255));
        if (ch < 3) {
            rgb_to_hsv(&k);
        }
    }
    if (memcmp(k.c, c->col.c, sizeof k.c) || k.h != c->col.h) {
        apply(m, c, L, obj, &k, step);
    }
}

/** @brief tfbus_driver_t::io_execute: LEDRGB methods (interface method indices). */
static int io_execute(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int first,
                      int nargs)
{
    if (cls != CLS_LEDRGB || ch != 0) {
        return -1;
    }
    rgbw_t *c = ctx(m);
    color_t k = c->col;
    uint32_t ramp;
    switch (idx) {
    case X_VALUE: {
        double v = luaL_checknumber(L, first);
        if ((v < c->min || v > c->max) && c->name[0]) {
            clu_lua_post_event(c->name, EV_RANGE);
        }
        k.v = light_clamp(v, c->min, c->max);
        hsv_to_rgb(&k);
        break;
    }
    case X_HUE:
        k.h = light_clamp(luaL_checknumber(L, first), 0, 360);
        hsv_to_rgb(&k);
        break;
    case X_SAT:
        k.s = light_clamp(luaL_checknumber(L, first), 0, 1);
        hsv_to_rgb(&k);
        break;
    case X_RED:
    case X_GREEN:
    case X_BLUE:
        k.c[idx - X_RED] = (uint8_t)lround(light_clamp(luaL_checknumber(L, first), 0, 255));
        rgb_to_hsv(&k);
        break;
    case X_WHITE:
        k.c[3] = (uint8_t)lround(light_clamp(luaL_checknumber(L, first), 0, 255));
        break;
    case X_RGB:
        if (!parse_rgb(luaL_checkstring(L, first), &k)) {
            return luaL_error(L, "SetRGBvalue: expected \"#RRGGBB\"");
        }
        rgb_to_hsv(&k);
        break;
    case X_HOLD:
    case X_HOLD_HUE:
    case X_HOLD_RED:
    case X_HOLD_RED + 1:
    case X_HOLD_RED + 2:
    case X_HOLD_WHITE:
        light_timer_cancel(&c->timed);
        hold(m, c, L, obj, idx, light_ramp_arg(L, first, nargs, 0, c->ramp));
        return 0;
    case X_ON:
    case X_OFF:
    case X_SWITCH: {
        double ms = light_arg(L, first, nargs, 0, 0);
        ramp = light_ramp_arg(L, first, nargs, 1, c->ramp);
        bool on = idx == X_ON || (idx == X_SWITCH && !lit(&c->col));
        k.v = on ? c->max : 0;
        hsv_to_rgb(&k);
        k.c[3] = on ? c->w_on : 0;
        light_timer_cancel(&c->timed);
        if (ms > 0 && memcmp(k.c, c->col.c, sizeof k.c)) {
            c->revert_to = c->col;
            c->revert_ramp = ramp;
            light_timer_start(&c->timed, m, c, revert, (uint32_t)ms);
        }
        apply(m, c, L, obj, &k, ramp);
        return 0;
    }
    default:
        return -1;
    }
    // Set* methods: Value, Ramp
    ramp = light_ramp_arg(L, first, nargs, 1, c->ramp);
    light_timer_cancel(&c->timed);
    apply(m, c, L, obj, &k, ramp);
    return 0;
}

/** @brief PowerSupplyVoltage update: Value / Value% and events. */
static void psu_update(rgbw_t *c, double u)
{
    const char *n = c->psu;
    bool first = !c->u_have;
    c->u_have = true;
    c->u = u;
    bool out = u < c->u_min || u > c->u_max;
    bool change = first || fabs(u - c->u_event) >= c->u_sens;
    if (n[0] && (change || out != c->u_out)) {
        clu_lua_post_number(n, P_VALUE, u);
        clu_lua_post_number(n, P_PERCENT, c->u_max > 0 ? round(u / c->u_max * 10000) / 100 : 0);
    }
    if (n[0] && change && !first) {
        clu_lua_post_event(n, PE_CHANGE);
        clu_lua_post_event(n, u < c->u_event ? PE_LOWER : PE_RISE);
    }
    if (change) {
        c->u_event = u;
    }
    if (n[0] && out != c->u_out && !(first && !out)) {
        clu_lua_post_event(n, out ? PE_OUT : PE_IN);
    }
    c->u_out = out;
}

/** @brief tfbus_driver_t::on_status: supply voltage. */
static void on_status(tfbus_module_t *m, const uint8_t *pl, size_t len)
{
    rgbw_t *c = ctx(m);
    tfbus_tlv_t r[4];
    int n = tfbus_parse_tlv(pl, len, r, 4);
    for (int i = 0; i < n; i++) {
        if (r[i].type == 0x02 && r[i].len >= 2) {
            psu_update(c, (r[i].data[0] | r[i].data[1] << 8) / 100.0);
        }
    }
}

const tfbus_driver_t tfbus_drv_led_rgbw_din = {
    .hw_type = 0x18,
    .name = "LED_RGBW_DIN",
    .init = init,
    .deinit = deinit,
    .on_status = on_status,
    .io_new = io_new,
    .io_set = io_set,
    .io_execute = io_execute,
};
