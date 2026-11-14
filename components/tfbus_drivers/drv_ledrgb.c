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
static rgbw_t *ctx(tfbus_module_t *module)
{
    if (!module->drv_ctx) {
        rgbw_t *rgbw_state = calloc(1, sizeof *rgbw_state);
        if (!rgbw_state) {
            abort(); // out of memory while loading OM.LUA
        }
        rgbw_state->max = 1;
        rgbw_state->corr[0] = rgbw_state->corr[1] = rgbw_state->corr[2] = 100;
        rgbw_state->u_sens = 0.5;
        rgbw_state->u_min = 21;
        rgbw_state->u_max = 24;
        rgbw_state->lch = ledch_new();
        module->drv_ctx = rgbw_state;
    }
    return module->drv_ctx;
}

// ---------------------------------------------------------------- colour model
/** @brief Truncate to two decimals (as the real CLU keeps Value / Saturation). */
static double trunc2(double value)
{
    return floor(value * 100 + 1e-6) / 100;
}

/** @brief 0..1 -> 0..255, truncated. */
static uint8_t to255(double normalized_value)
{
    return (uint8_t)light_clamp(floor(normalized_value * 255 + 1e-6), 0, 255);
}

/** @brief R/G/B from H/S/V (W untouched). */
static void hsv_to_rgb(color_t *color)
{
    double hue_sector = fmod(color->h, 360) / 60, saturation = color->s, value = color->v;
    int sector_index = (int)hue_sector;
    double sector_fraction = hue_sector - sector_index, low_component = value * (1 - saturation), falling_component = value * (1 - saturation * sector_fraction), rising_component = value * (1 - saturation * (1 - sector_fraction));
    double rgb[6][3] = {{value, rising_component, low_component}, {falling_component, value, low_component}, {low_component, value, rising_component}, {low_component, falling_component, value}, {rising_component, low_component, value}, {value, low_component, falling_component}};
    for (int j = 0; j < 3; j++) {
        color->c[j] = to255(rgb[sector_index % 6][j]);
    }
}

/** @brief H/S/V from R/G/B (Hue whole degrees, S/V two decimals); hue kept where undefined. */
static void rgb_to_hsv(color_t *color)
{
    double red_component = color->c[0] / 255.0, green_component = color->c[1] / 255.0, blue_component = color->c[2] / 255.0;
    double maximum = fmax(red_component, fmax(green_component, blue_component)), minimum = fmin(red_component, fmin(green_component, blue_component)), chroma = maximum - minimum;
    color->v = trunc2(maximum);
    color->s = maximum == 0 ? 0 : trunc2(chroma / maximum);
    if (chroma == 0) {
        return; // black / grey: keep hue
    }
    // in float like the real CLU: (255, 127, 63) gives 19.99999 -> 19, not 20
    float red_float = color->c[0] / 255.0f, green_float = color->c[1] / 255.0f, blue_float = color->c[2] / 255.0f;
    float float_chroma = fmaxf(red_float, fmaxf(green_float, blue_float)) - fminf(red_float, fminf(green_float, blue_float));
    float hue_degrees = maximum == red_component ? fmodf((green_float - blue_float) / float_chroma, 6) : maximum == green_component ? (blue_float - red_float) / float_chroma + 2 : (red_float - green_float) / float_chroma + 4;
    hue_degrees *= 60;
    color->h = floorf(hue_degrees < 0 ? hue_degrees + 360 : hue_degrees);
}

/** @brief Set Value / Hue / Saturation (already in range) and recompute R/G/B. */
static void set_hsv(color_t *color, double hue_degrees, double saturation, double value)
{
    color->h = hue_degrees;
    color->s = saturation;
    color->v = value;
    hsv_to_rgb(color);
}

static bool lit(const color_t *color)
{
    return color->c[0] || color->c[1] || color->c[2] || color->c[3];
}

/** @brief "#RRGGBB" of the R/G/B components. */
static void rgb_str(const color_t *color, char out[8])
{
    snprintf(out, 8, "#%02X%02X%02X", color->c[0], color->c[1], color->c[2]);
}

// ---------------------------------------------------------------- output
/** @brief Queue SET_RAMP_TIME for all four channels (two frames: 3, 2 then 1, 0; 10 ms units). */
static void send_ramp(tfbus_module_t *module, uint32_t ms)
{
    unsigned ramp_units = (ms > LIGHT_RAMP_MAX ? LIGHT_RAMP_MAX : ms) / 10;
    uint8_t ramp_records[4][5];
    tfbus_tlv_t tlv_records[4];
    for (int i = 0; i < 4; i++) {
        uint8_t channel = (uint8_t)(3 - i);
        memcpy(ramp_records[i], (uint8_t[]){channel, (uint8_t)ramp_units, (uint8_t)(ramp_units >> 8), 0, 0}, 5);
        tlv_records[i] = (tfbus_tlv_t){.type = 0x06, .meta = 0x00, .len = 5, .data = ramp_records[i]};
    }
    drv_queue_records(module, tlv_records, 2);
    drv_queue_records(module, tlv_records + 2, 2);
}

/** @brief Queue SET_CHANNEL_VALUE x4. */
static void send_values(tfbus_module_t *module, const rgbw_t *rgbw_state)
{
    uint8_t value_records[4][2];
    tfbus_tlv_t tlv_records[4];
    for (int i = 0; i < 4; i++) {
        value_records[i][0] = (uint8_t)i;
        value_records[i][1] = rgbw_state->col.c[i];
        tlv_records[i] = (tfbus_tlv_t){.type = 0x05, .meta = 0x10, .len = 2, .data = value_records[i]};
    }
    drv_queue_records(module, tlv_records, 4);
}

/** @brief Queue a channel correction (07 03 10 [ch, % LE u16]). */
static void send_corr(tfbus_module_t *module, int channel, uint8_t pct)
{
    drv_queue_record(module, 0x07, 0x10, (uint8_t[]){(uint8_t)channel, pct, 0}, 3);
}

/** @brief Report the colour features: stored on the object (Lua hook, L != NULL) or posted (bus task). */
static void publish(const rgbw_t *rgbw_state, lua_State *L, int obj)
{
    static const int idx[] = {F_VALUE, F_HUE, F_SAT, F_RED, F_GREEN, F_BLUE, F_WHITE};
    const color_t *color = &rgbw_state->col;
    double val[] = {color->v, color->h, color->s, color->c[0], color->c[1], color->c[2], color->c[3]};
    char hex[8];
    rgb_str(color, hex);
    if (L) {
        for (int i = 0; i < 7; i++) {
            tfbus_obj_store(L, obj, idx[i], val[i]);
        }
        lua_getfield(L, obj, "_f");
        lua_pushstring(L, hex);
        lua_rawseti(L, -2, F_RGB);
        lua_pop(L, 1);
    } else if (rgbw_state->name[0]) {
        for (int i = 0; i < 7; i++) {
            clu_lua_post_number(rgbw_state->name, idx[i], val[i]);
        }
        clu_lua_post_string(rgbw_state->name, F_RGB, hex);
    }
}

/** @brief Fire the events for a colour change @p old -> current. */
static void events(const rgbw_t *rgbw_state, const color_t *old)
{
    const color_t *current_color = &rgbw_state->col;
    if (!rgbw_state->name[0] || !memcmp(old->c, current_color->c, sizeof current_color->c)) {
        return;
    }
    clu_lua_post_event(rgbw_state->name, EV_CHANGE);
    if (current_color->v != old->v) {
        clu_lua_post_event(rgbw_state->name, current_color->v > old->v ? EV_RISE : EV_LOWER);
    }
    if (!lit(old) && lit(current_color)) {
        clu_lua_post_event(rgbw_state->name, EV_ON);
    } else if (lit(old) && !lit(current_color)) {
        clu_lua_post_event(rgbw_state->name, EV_OFF);
    }
}

/** @brief No SET_RAMP_TIME before the values (timed switch expiry). */
#define NO_RAMP UINT32_MAX

/** @brief Drive the module to @p k and report it (see publish() for @p L / @p obj). */
static void apply(tfbus_module_t *module, rgbw_t *rgbw_state, lua_State *L, int obj, const color_t *target_color, uint32_t ramp)
{
    color_t old = rgbw_state->col;
    rgbw_state->col = *target_color;
    rgbw_state->have_target = true;
    if (ramp != NO_RAMP) {
        send_ramp(module, ramp);
    }
    send_values(module, rgbw_state);
    publish(rgbw_state, L, obj);
    events(rgbw_state, &old);
}

/** @brief Timed switch expired (bus task): restore the previous colour. */
static void revert(tfbus_module_t *module, void *owner)
{
    rgbw_t *rgbw_state = owner;
    apply(module, rgbw_state, NULL, 0, &rgbw_state->revert_to, NO_RAMP);
}

// ---------------------------------------------------------------- driver hooks
/** @brief tfbus_driver_t::deinit: stop the timers, free the channel objects (the master frees drv_ctx). */
static void deinit(tfbus_module_t *module)
{
    rgbw_t *rgbw_state = module->drv_ctx;
    light_timer_delete(&rgbw_state->timed);
    ledch_deinit(rgbw_state->lch);
    free(rgbw_state->lch);
    rgbw_state->lch = NULL;
}

/** @brief tfbus_driver_t::overdue: run expired timers whose deferred call was dropped. */
static void overdue(tfbus_module_t *module)
{
    rgbw_t *rgbw_state = module->drv_ctx;
    light_timer_overdue(&rgbw_state->timed);
    if (rgbw_state->lch) {
        ledch_overdue(rgbw_state->lch);
    }
}

/** @brief tfbus_driver_t::io_new: remember the LEDRGB / PowerSupplyVoltage names. */
static void io_new(tfbus_module_t *module, int cls, int channel, const char *name)
{
    rgbw_t *rgbw_state = ctx(module);
    if (cls == CLS_LEDRGB && channel == 0) {
        strlcpy(rgbw_state->name, name, sizeof rgbw_state->name);
    } else if (cls == CLS_PSU && channel == 0) {
        strlcpy(rgbw_state->psu, name, sizeof rgbw_state->psu);
    } else {
        ledch_io_new(rgbw_state->lch, cls, channel, name);
    }
}

/** @brief tfbus_driver_t::init: re-apply corrections and the colour Lua set (module re-addressed / back online). */
static void init(tfbus_module_t *module)
{
    rgbw_t *rgbw_state = ctx(module);
    for (int i = 0; i < 3; i++) {
        if (rgbw_state->corr[i] != 100) {
            send_corr(module, i, rgbw_state->corr[i]);
        }
    }
    if (rgbw_state->have_target) {
        send_ramp(module, 0);
        send_values(module, rgbw_state);
    }
    ledch_init(module, rgbw_state->lch);
}

/**
 * @brief tfbus_driver_t::io_set: RampTime and corrections go to the module at once; Min / MaxValue
 *        and PSU limits are cached.
 */
static bool io_set(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel, int idx, int val)
{
    (void)obj;
    rgbw_t *rgbw_state = ctx(module);
    if (cls == LEDCH_CLS_CHANNEL || cls == LEDCH_CLS_CCT) {
        return ledch_io_set(module, rgbw_state->lch, L, cls, channel, idx, val);
    }
    if (channel != 0) {
        return false;
    }
    if (cls == CLS_PSU) {
        double supply_voltage = luaL_checknumber(L, val);
        if (idx == P_SENS) {
            rgbw_state->u_sens = supply_voltage;
        } else if (idx == P_MIN) {
            rgbw_state->u_min = supply_voltage;
        } else if (idx == P_MAX) {
            rgbw_state->u_max = supply_voltage;
        }
        return false;
    }
    if (cls != CLS_LEDRGB) {
        return false;
    }
    switch (idx) {
    case F_RAMP:
        rgbw_state->ramp = (uint32_t)light_clamp(luaL_checknumber(L, val), 0, LIGHT_RAMP_MAX);
        send_ramp(module, rgbw_state->ramp);
        break;
    case F_MIN:
        rgbw_state->min = light_clamp(luaL_checknumber(L, val), 0, 1);
        break;
    case F_MAX:
        rgbw_state->max = light_clamp(luaL_checknumber(L, val), 0, 1);
        break;
    case F_RCORR:
    case F_GCORR:
    case F_BCORR: {
        rgbw_state->corr[idx - F_RCORR] = (uint8_t)light_clamp(luaL_checknumber(L, val), 0, 100);
        send_corr(module, idx - F_RCORR, rgbw_state->corr[idx - F_RCORR]);
        break;
    }
    default:
        break;
    }
    return false; // store generically
}

/** @brief Parse "#RRGGBB" / "RRGGBB" into k->c[0..2]; false if malformed. */
static bool parse_rgb(const char *rgb_text, color_t *color)
{
    if (*rgb_text == '#') {
        rgb_text++;
    }
    if (strlen(rgb_text) != 6 || strspn(rgb_text, "0123456789abcdefABCDEF") != 6) {
        return false;
    }
    unsigned long rgb_value = strtoul(rgb_text, NULL, 16);
    color->c[0] = (uint8_t)(rgb_value >> 16);
    color->c[1] = (uint8_t)(rgb_value >> 8);
    color->c[2] = (uint8_t)rgb_value;
    return true;
}

/** @brief Hold* methods: one step of Value / Hue / a channel; @p sweep = full-range time [ms]. */
static void hold(tfbus_module_t *module, rgbw_t *rgbw_state, lua_State *L, int obj, int idx, uint32_t sweep)
{
    color_t target_color = rgbw_state->col;
    int64_t now = esp_timer_get_time();
    uint32_t ramp = sweep;
    if (idx == X_HOLD) {
        int dir = light_hold_dir(&rgbw_state->hold, now, target_color.v, rgbw_state->min, rgbw_state->max);
        double value_step = sweep ? trunc2((double)LIGHT_HOLD_STEP_MS / sweep) : 1;
        set_hsv(&target_color, target_color.h, target_color.s, trunc2(light_clamp(target_color.v + dir * value_step, rgbw_state->min, rgbw_state->max)));
    } else if (idx == X_HOLD_HUE) {
        int dir = light_hold_dir(&rgbw_state->hold, now, target_color.h, -1, 361); // no ends: wraps
        double hue_value = target_color.h + dir * (sweep ? floor(360.0 * LIGHT_HOLD_STEP_MS / sweep) : 60);
        set_hsv(&target_color, hue_value < 0 ? 359 : hue_value > 359 ? 0 : hue_value, target_color.s, target_color.v);
        ramp = 0; // captured: HoldHue sends ramp 0
    } else {
        int channel_index = idx == X_HOLD_WHITE ? 3 : idx - X_HOLD_RED; // 0 R, 1 G, 2 B, 3 W
        int dir = light_hold_dir(&rgbw_state->hold, now, target_color.c[channel_index], 0, 255);
        double value_step = sweep ? lround(255.0 * LIGHT_HOLD_STEP_MS / sweep) : 255;
        target_color.c[channel_index] = (uint8_t)light_clamp(target_color.c[channel_index] + dir * value_step, 0, 255);
        if (channel_index < 3) {
            rgb_to_hsv(&target_color);
        }
    }
    apply(module, rgbw_state, L, obj, &target_color, ramp);
}

/** @brief tfbus_driver_t::io_execute: LEDRGB methods (interface method indices). */
static int io_execute(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel, int idx, int first,
                      int nargs)
{
    if (cls == LEDCH_CLS_CHANNEL || cls == LEDCH_CLS_CCT) {
        return ledch_io_execute(module, ctx(module)->lch, L, obj, cls, channel, idx, first, nargs);
    }
    if (cls != CLS_LEDRGB || channel != 0) {
        return -1;
    }
    rgbw_t *rgbw_state = ctx(module);
    color_t target_color = rgbw_state->col;
    uint32_t ramp;
    switch (idx) {
    case X_VALUE: {
        double requested_value = luaL_checknumber(L, first);
        if ((requested_value < rgbw_state->min || requested_value > rgbw_state->max) && rgbw_state->name[0]) {
            clu_lua_post_event(rgbw_state->name, EV_RANGE);
        }
        set_hsv(&target_color, target_color.h, target_color.s, light_clamp(requested_value, rgbw_state->min, rgbw_state->max));
        break;
    }
    case X_HUE:
        set_hsv(&target_color, floor(light_clamp(luaL_checknumber(L, first), 0, 360)), target_color.s, target_color.v);
        break;
    case X_SAT:
        set_hsv(&target_color, target_color.h, light_clamp(luaL_checknumber(L, first), 0, 1), target_color.v);
        break;
    case X_RED:
    case X_GREEN:
    case X_BLUE:
        target_color.c[idx - X_RED] = (uint8_t)light_clamp(luaL_checknumber(L, first), 0, 255);
        rgb_to_hsv(&target_color);
        break;
    case X_WHITE:
        target_color.c[3] = (uint8_t)light_clamp(luaL_checknumber(L, first), 0, 255);
        break;
    case X_RGB:
        if (!parse_rgb(luaL_checkstring(L, first), &target_color)) {
            return luaL_error(L, "SetRGBvalue: expected \"#RRGGBB\"");
        }
        rgb_to_hsv(&target_color);
        break;
    case X_HOLD:
    case X_HOLD_HUE:
    case X_HOLD_RED:
    case X_HOLD_RED + 1:
    case X_HOLD_RED + 2:
    case X_HOLD_WHITE:
        light_timer_cancel(&rgbw_state->timed);
        hold(module, rgbw_state, L, obj, idx, light_ramp_arg(L, first, nargs, 0, rgbw_state->ramp));
        return 0;
    case X_ON:
    case X_OFF:
    case X_SWITCH: {
        double ms = light_arg(L, first, nargs, 0, 0);
        ramp = light_ramp_arg(L, first, nargs, 1, rgbw_state->ramp);
        bool on = idx == X_ON || (idx == X_SWITCH && !lit(&rgbw_state->col));
        set_hsv(&target_color, target_color.h, target_color.s, on ? rgbw_state->max : 0);
        if (!on) {
            target_color.c[3] = 0; // SwitchOff: W too; SwitchOn leaves W as it is
        }
        light_timer_cancel(&rgbw_state->timed);
        if (ms > 0 && memcmp(target_color.c, rgbw_state->col.c, sizeof target_color.c)) {
            rgbw_state->revert_to = rgbw_state->col;
            light_timer_start(&rgbw_state->timed, module, rgbw_state, revert, (uint32_t)ms);
        }
        apply(module, rgbw_state, L, obj, &target_color, ramp);
        return 0;
    }
    default:
        return -1;
    }
    // Set* methods: Value, Ramp
    ramp = light_ramp_arg(L, first, nargs, 1, rgbw_state->ramp);
    light_timer_cancel(&rgbw_state->timed);
    apply(module, rgbw_state, L, obj, &target_color, ramp);
    return 0;
}

/** @brief PowerSupplyVoltage update: Value / Value% and events. */
static void psu_update(rgbw_t *rgbw_state, double supply_voltage)
{
    const char *object_name = rgbw_state->psu;
    bool first = !rgbw_state->u_have;
    rgbw_state->u_have = true;
    rgbw_state->u = supply_voltage;
    bool out = supply_voltage < rgbw_state->u_min || supply_voltage > rgbw_state->u_max;
    bool change = first || fabs(supply_voltage - rgbw_state->u_event) >= rgbw_state->u_sens;
    if (object_name[0] && (change || out != rgbw_state->u_out)) {
        clu_lua_post_number(object_name, P_VALUE, supply_voltage);
        clu_lua_post_number(object_name, P_PERCENT, rgbw_state->u_max > 0 ? round(supply_voltage / rgbw_state->u_max * 10000) / 100 : 0);
    }
    if (object_name[0] && change && !first) {
        clu_lua_post_event(object_name, PE_CHANGE);
        clu_lua_post_event(object_name, supply_voltage < rgbw_state->u_event ? PE_LOWER : PE_RISE);
    }
    if (change) {
        rgbw_state->u_event = supply_voltage;
    }
    if (object_name[0] && out != rgbw_state->u_out && !(first && !out)) {
        clu_lua_post_event(object_name, out ? PE_OUT : PE_IN);
    }
    rgbw_state->u_out = out;
}

/** @brief tfbus_driver_t::on_status: supply voltage, channel values (LED_CHANNEL / LED_CCT). */
static void on_status(tfbus_module_t *module, const uint8_t *status_payload, size_t len)
{
    rgbw_t *rgbw_state = ctx(module);
    tfbus_tlv_t status_records[8];
    int status_record_count = tfbus_parse_tlv(status_payload, len, status_records, 8);
    for (int i = 0; i < status_record_count; i++) {
        if (status_records[i].type == 0x02 && status_records[i].len >= 2) {
            psu_update(rgbw_state, (status_records[i].data[0] | status_records[i].data[1] << 8) / 100.0);
        } else if (status_records[i].type == 0x04 && status_records[i].len >= 2) {
            ledch_reported(rgbw_state->lch, status_records[i].data[0], status_records[i].data[1]);
        }
    }
    ledch_reported_end(rgbw_state->lch);
}

const tfbus_driver_t tfbus_drv_led_rgbw_din = {
    .hw_type = 0x18,
    .name = "LED_RGBW_DIN",
    .init = init,
    .deinit = deinit,
    .overdue = overdue,
    .on_status = on_status,
    .io_new = io_new,
    .io_set = io_set,
    .io_execute = io_execute,
};
