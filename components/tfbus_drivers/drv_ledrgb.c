/**
 * @file drv_ledrgb.c
 * @brief LED_RGBW_DIN (hwType 24 / 0x18): 1x LEDRGB (class 9), 1x PowerSupplyVoltage (class 12),
 *        4x LED_CHANNEL (class 49), 2x LED_CCT (class 72; drv_ledch.c).
 *
 * Interface module_2_0_LED_RGBW_DIN_fv04_02.xml. Wire format and method behaviour from captures of
 * a real CLU (CLU_2, module addr 5):
 * @verbatim
 *   Set* / Switch* / Hold* : SET_RAMP_TIME per channel, two frames of two records
 *                              06 05 00 [3, t LE u16, 0, 0]  06 05 00 [2, ...]  then  [1], [0]
 *                              (t in 10 ms: 1000 ms -> 100)
 *                            then one frame SET_CHANNEL_VALUE 05 02 10 [ch, value 0-255] x4
 *                              (ch 0 R, 1 G, 2 B, 3 W; values as set, not corrected)
 *   timed Switch* expiry   : SET_CHANNEL_VALUE frame only
 *   SetRampTime            : the SET_RAMP_TIME frames at once
 *   Red/Green/BlueCorrection: 07 03 10 [ch, % LE u16]   (the module corrects)
 *   Min/MaxValue           : nothing (clamped in the CLU)
 *   status                 : 01 | 02 02 10 [U LE u16, 0.01 V]          (PowerSupplyVoltage)
 *                            n | 04 02 08 [ch, value] ...              (channels, after a change;
 *                                                                       used by LED_CHANNEL / LED_CCT)
 * @endverbatim
 * Every call sends its frames, also when nothing changes. The real CLU sends nothing to these
 * modules at startup (refresh capture).
 *
 * The colour model lives in the CLU (captured):
 * - State is HSV + R/G/B + W (independent). Hue is kept as a whole degree, Saturation / Value with
 *   two decimals (truncated), R/G/B = trunc(component * 255) recomputed from those. Setting R/G/B or
 *   the RGB string recomputes HSV (black / grey: Saturation 0, Hue kept).
 * - Ramp argument absent = RampTime; given (also 0) = sent as is.
 * - SetValue clamps to [MinValue, MaxValue]; SwitchOn -> MaxValue (W unchanged), SwitchOff -> all
 *   channels incl. W 0; Switch toggles by "any channel on"; Time > 0 restores the previous colour.
 * - Hold*(Ramp): one step per call (100/Ramp of the range), one direction shared by all Hold*
 *   methods (see light_hold_dir()). HoldValue: Value +-trunc(100/Ramp, 2 decimals) within
 *   [MinValue, MaxValue]; HoldHue: +-trunc(36000/Ramp) degrees, 0 <-> 359 wrap, sends ramp 0;
 *   Hold<Channel>: +-round(25500/Ramp) clamped to 0-255.
 * Not checked against a real CLU:
 * - Events: OnValueChange on any channel change; OnValueRise / OnValueLower by Value; OnSwitchOn /
 *   OnSwitchOff when the output goes from / to all channels 0; OnOutOfRange.
 * - StatisticState / Load: stored only.
 * PowerSupplyVoltage: Value, Value% (of MaxValue), events by Sensitivity, OnOutOfRange / OnInRange.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "drv_ledch.h"
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
    double min, max;
    uint8_t corr[3];       // R/G/B correction [%]
    uint32_t ramp;         // RampTime [ms]
    bool have_target;      // Lua set a colour (re-applied by init)
    light_timer_t timed;   // Switch* with Time > 0
    color_t revert_to;
    light_hold_t hold;     // shared by all Hold* methods

    // PowerSupplyVoltage
    double u, u_event;     // last value, value at the last change event
    double u_sens, u_min, u_max;
    bool u_have, u_out;

    ledch_t *lch;          // LED_CHANNEL / LED_CCT objects (drv_ledch.c)
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
        c->lch = ledch_new();
        m->drv_ctx = c;
    }
    return m->drv_ctx;
}

// ---------------------------------------------------------------- colour model
/** @brief Truncate to two decimals (as the real CLU keeps Value / Saturation). */
static double trunc2(double x)
{
    return floor(x * 100 + 1e-6) / 100;
}

/** @brief 0..1 -> 0..255, truncated. */
static uint8_t to255(double x)
{
    return (uint8_t)light_clamp(floor(x * 255 + 1e-6), 0, 255);
}

/** @brief R/G/B from H/S/V (W untouched). */
static void hsv_to_rgb(color_t *k)
{
    double h = fmod(k->h, 360) / 60, s = k->s, v = k->v;
    int i = (int)h;
    double f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    double rgb[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
    for (int j = 0; j < 3; j++) {
        k->c[j] = to255(rgb[i % 6][j]);
    }
}

/** @brief H/S/V from R/G/B (Hue whole degrees, S/V two decimals); hue kept where undefined. */
static void rgb_to_hsv(color_t *k)
{
    double r = k->c[0] / 255.0, g = k->c[1] / 255.0, b = k->c[2] / 255.0;
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), d = mx - mn;
    k->v = trunc2(mx);
    k->s = mx == 0 ? 0 : trunc2(d / mx);
    if (d == 0) {
        return; // black / grey: keep hue
    }
    // in float like the real CLU: (255, 127, 63) gives 19.99999 -> 19, not 20
    float rf = k->c[0] / 255.0f, gf = k->c[1] / 255.0f, bf = k->c[2] / 255.0f;
    float df = fmaxf(rf, fmaxf(gf, bf)) - fminf(rf, fminf(gf, bf));
    float h = mx == r ? fmodf((gf - bf) / df, 6) : mx == g ? (bf - rf) / df + 2 : (rf - gf) / df + 4;
    h *= 60;
    k->h = floorf(h < 0 ? h + 360 : h);
}

/** @brief Set Value / Hue / Saturation (already in range) and recompute R/G/B. */
static void set_hsv(color_t *k, double h, double s, double v)
{
    k->h = h;
    k->s = s;
    k->v = v;
    hsv_to_rgb(k);
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
/** @brief Queue SET_RAMP_TIME for all four channels (two frames: 3, 2 then 1, 0; 10 ms units). */
static void send_ramp(tfbus_module_t *m, uint32_t ms)
{
    unsigned t = (ms > LIGHT_RAMP_MAX ? LIGHT_RAMP_MAX : ms) / 10;
    uint8_t d[4][5];
    tfbus_tlv_t r[4];
    for (int i = 0; i < 4; i++) {
        uint8_t ch = (uint8_t)(3 - i);
        memcpy(d[i], (uint8_t[]){ch, (uint8_t)t, (uint8_t)(t >> 8), 0, 0}, 5);
        r[i] = (tfbus_tlv_t){.type = 0x06, .meta = 0x00, .len = 5, .data = d[i]};
    }
    drv_queue_records(m, r, 2);
    drv_queue_records(m, r + 2, 2);
}

/** @brief Queue SET_CHANNEL_VALUE x4. */
static void send_values(tfbus_module_t *m, const rgbw_t *c)
{
    uint8_t d[4][2];
    tfbus_tlv_t r[4];
    for (int i = 0; i < 4; i++) {
        d[i][0] = (uint8_t)i;
        d[i][1] = c->col.c[i];
        r[i] = (tfbus_tlv_t){.type = 0x05, .meta = 0x10, .len = 2, .data = d[i]};
    }
    drv_queue_records(m, r, 4);
}

/** @brief Queue a channel correction (07 03 10 [ch, % LE u16]). */
static void send_corr(tfbus_module_t *m, int ch, uint8_t pct)
{
    drv_queue_record(m, 0x07, 0x10, (uint8_t[]){(uint8_t)ch, pct, 0}, 3);
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

/** @brief No SET_RAMP_TIME before the values (timed switch expiry). */
#define NO_RAMP UINT32_MAX

/** @brief Drive the module to @p k and report it (see publish() for @p L / @p obj). */
static void apply(tfbus_module_t *m, rgbw_t *c, lua_State *L, int obj, const color_t *k, uint32_t ramp)
{
    color_t old = c->col;
    c->col = *k;
    c->have_target = true;
    if (ramp != NO_RAMP) {
        send_ramp(m, ramp);
    }
    send_values(m, c);
    publish(c, L, obj);
    events(c, &old);
}

/** @brief Timed switch expired (bus task): restore the previous colour. */
static void revert(tfbus_module_t *m, void *owner)
{
    rgbw_t *c = owner;
    apply(m, c, NULL, 0, &c->revert_to, NO_RAMP);
}

// ---------------------------------------------------------------- driver hooks
/** @brief tfbus_driver_t::deinit: stop the timers, free the channel objects (the master frees drv_ctx). */
static void deinit(tfbus_module_t *m)
{
    rgbw_t *c = m->drv_ctx;
    light_timer_delete(&c->timed);
    ledch_deinit(c->lch);
    free(c->lch);
    c->lch = NULL;
}

/** @brief tfbus_driver_t::io_new: remember the LEDRGB / PowerSupplyVoltage names. */
static void io_new(tfbus_module_t *m, int cls, int ch, const char *name)
{
    rgbw_t *c = ctx(m);
    if (cls == CLS_LEDRGB && ch == 0) {
        strlcpy(c->name, name, sizeof c->name);
    } else if (cls == CLS_PSU && ch == 0) {
        strlcpy(c->psu, name, sizeof c->psu);
    } else {
        ledch_io_new(c->lch, cls, ch, name);
    }
}

/** @brief tfbus_driver_t::init: re-apply corrections and the colour Lua set (module re-addressed / back online). */
static void init(tfbus_module_t *m)
{
    rgbw_t *c = ctx(m);
    for (int i = 0; i < 3; i++) {
        if (c->corr[i] != 100) {
            send_corr(m, i, c->corr[i]);
        }
    }
    if (c->have_target) {
        send_ramp(m, 0);
        send_values(m, c);
    }
    ledch_init(m, c->lch);
}

/**
 * @brief tfbus_driver_t::io_set: RampTime and corrections go to the module at once; Min / MaxValue
 *        and PSU limits are cached.
 */
static bool io_set(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int val)
{
    (void)obj;
    rgbw_t *c = ctx(m);
    if (cls == LEDCH_CLS_CHANNEL || cls == LEDCH_CLS_CCT) {
        return ledch_io_set(m, c->lch, L, cls, ch, idx, val);
    }
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
        send_ramp(m, c->ramp);
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
        c->corr[idx - F_RCORR] = (uint8_t)light_clamp(luaL_checknumber(L, val), 0, 100);
        send_corr(m, idx - F_RCORR, c->corr[idx - F_RCORR]);
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

/** @brief Hold* methods: one step of Value / Hue / a channel; @p sweep = full-range time [ms]. */
static void hold(tfbus_module_t *m, rgbw_t *c, lua_State *L, int obj, int idx, uint32_t sweep)
{
    color_t k = c->col;
    int64_t now = esp_timer_get_time();
    uint32_t ramp = sweep;
    if (idx == X_HOLD) {
        int dir = light_hold_dir(&c->hold, now, k.v, c->min, c->max);
        double d = sweep ? trunc2((double)LIGHT_HOLD_STEP_MS / sweep) : 1;
        set_hsv(&k, k.h, k.s, trunc2(light_clamp(k.v + dir * d, c->min, c->max)));
    } else if (idx == X_HOLD_HUE) {
        int dir = light_hold_dir(&c->hold, now, k.h, -1, 361); // no ends: wraps
        double h = k.h + dir * (sweep ? floor(360.0 * LIGHT_HOLD_STEP_MS / sweep) : 60);
        set_hsv(&k, h < 0 ? 359 : h > 359 ? 0 : h, k.s, k.v);
        ramp = 0; // captured: HoldHue sends ramp 0
    } else {
        int ch = idx == X_HOLD_WHITE ? 3 : idx - X_HOLD_RED; // 0 R, 1 G, 2 B, 3 W
        int dir = light_hold_dir(&c->hold, now, k.c[ch], 0, 255);
        double d = sweep ? lround(255.0 * LIGHT_HOLD_STEP_MS / sweep) : 255;
        k.c[ch] = (uint8_t)light_clamp(k.c[ch] + dir * d, 0, 255);
        if (ch < 3) {
            rgb_to_hsv(&k);
        }
    }
    apply(m, c, L, obj, &k, ramp);
}

/** @brief tfbus_driver_t::io_execute: LEDRGB methods (interface method indices). */
static int io_execute(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int first,
                      int nargs)
{
    if (cls == LEDCH_CLS_CHANNEL || cls == LEDCH_CLS_CCT) {
        return ledch_io_execute(m, ctx(m)->lch, L, obj, cls, ch, idx, first, nargs);
    }
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
        set_hsv(&k, k.h, k.s, light_clamp(v, c->min, c->max));
        break;
    }
    case X_HUE:
        set_hsv(&k, floor(light_clamp(luaL_checknumber(L, first), 0, 360)), k.s, k.v);
        break;
    case X_SAT:
        set_hsv(&k, k.h, light_clamp(luaL_checknumber(L, first), 0, 1), k.v);
        break;
    case X_RED:
    case X_GREEN:
    case X_BLUE:
        k.c[idx - X_RED] = (uint8_t)light_clamp(luaL_checknumber(L, first), 0, 255);
        rgb_to_hsv(&k);
        break;
    case X_WHITE:
        k.c[3] = (uint8_t)light_clamp(luaL_checknumber(L, first), 0, 255);
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
        set_hsv(&k, k.h, k.s, on ? c->max : 0);
        if (!on) {
            k.c[3] = 0; // SwitchOff: W too; SwitchOn leaves W as it is
        }
        light_timer_cancel(&c->timed);
        if (ms > 0 && memcmp(k.c, c->col.c, sizeof k.c)) {
            c->revert_to = c->col;
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

/** @brief tfbus_driver_t::on_status: supply voltage, channel values (LED_CHANNEL / LED_CCT). */
static void on_status(tfbus_module_t *m, const uint8_t *pl, size_t len)
{
    rgbw_t *c = ctx(m);
    tfbus_tlv_t r[8];
    int n = tfbus_parse_tlv(pl, len, r, 8);
    for (int i = 0; i < n; i++) {
        if (r[i].type == 0x02 && r[i].len >= 2) {
            psu_update(c, (r[i].data[0] | r[i].data[1] << 8) / 100.0);
        } else if (r[i].type == 0x04 && r[i].len >= 2) {
            ledch_reported(c->lch, r[i].data[0], r[i].data[1]);
        }
    }
    ledch_reported_end(c->lch);
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
