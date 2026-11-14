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
static roller_t *ctx(tfbus_module_t *module)
{
    if (!module->drv_ctx) {
        roller_t *roller_state = calloc(1, sizeof *roller_state);
        if (!roller_state) {
            abort(); // out of memory while loading OM.LUA: reboot instead of a NULL dereference
        }
        roller_state->m = module;
        roller_state->cfg = module->drv->cfg;
        roller_state->active_ch = -1;
        for (int i = 0; i < CH; i++) {
            roller_state->max_ms[i] = 30000;
            roller_state->pos[i] = -1;
            roller_state->last_dir[i] = ST_DOWN;
        }
        module->drv_ctx = roller_state;
    }
    return module->drv_ctx;
}

/**
 * @brief Build a FEATURE_PARAM record [idx, n, time LE u16 in 10 ms units] (saturates at 0xFFFF).
 */
static void param(uint8_t out[4], uint8_t idx, int channel_number, uint32_t ms)
{
    uint32_t scaled_time = ms / 10;
    if (scaled_time > 0xFFFF) {
        scaled_time = 0xFFFF;
    }
    out[0] = idx;
    out[1] = (uint8_t)channel_number;
    out[2] = (uint8_t)scaled_time;
    out[3] = (uint8_t)(scaled_time >> 8);
}

/** @brief Up travel time of channel @p channel_index (MaxTime if unset). */
static uint32_t up_time(roller_t *roller_state, int channel_index)
{
    return roller_state->up_ms[channel_index] ? roller_state->up_ms[channel_index] : roller_state->max_ms[channel_index];
}

/** @brief Down travel time of channel @p channel_index (MaxTime if unset). */
static uint32_t down_time(roller_t *roller_state, int channel_index)
{
    return roller_state->down_ms[channel_index] ? roller_state->down_ms[channel_index] : roller_state->max_ms[channel_index];
}

/** @brief Queue the startup init sequence of one channel (ROLLER_SH_DIN_3). */
static void init_channel(roller_t *roller_state, int channel_index)
{
    tfbus_module_t *module = roller_state->m;
    int wire_channel = channel_index + 1;
    uint8_t parameter_data[4];
    drv_queue_record(module, 0x04, 0x00, (uint8_t[]){0x16, (uint8_t)wire_channel, 0}, 3); // CHANNEL_INIT
    param(parameter_data, 0, wire_channel, roller_state->max_ms[channel_index]);
    drv_queue_record(module, 0x17, 0x00, parameter_data, 4);
    param(parameter_data, 2, wire_channel, up_time(roller_state, channel_index));
    drv_queue_record(module, 0x17, 0x00, parameter_data, 4);
    param(parameter_data, 3, wire_channel, down_time(roller_state, channel_index));
    drv_queue_record(module, 0x17, 0x00, parameter_data, 4);
    param(parameter_data, 1, wire_channel, roller_state->offset_ms[channel_index]);
    drv_queue_record(module, 0x17, 0x00, parameter_data, 4);
    drv_queue_record(module, 0x0f, 0x00, (uint8_t[]){(uint8_t)channel_index, 1, 0, 0}, 4);   // MODULE_PARAM
    drv_queue_record(module, 0x1c, 0x00, (uint8_t[]){0, (uint8_t)wire_channel}, 2);         // ROLLER_CHAN_REG
}

/**
 * @brief Queue the reconfiguration sequence of one channel after its timing changed (ROLLER_SH_DIN_3).
 */
static void reconfigure_channel(roller_t *roller_state, int channel_index)
{
    tfbus_module_t *module = roller_state->m;
    int wire_channel = channel_index + 1;
    uint8_t up_map_record[2] = {(uint8_t)(2 * wire_channel - 1), 0}, down_map_record[2] = {(uint8_t)(2 * wire_channel), 0};
    tfbus_tlv_t map[2] = {{.type = 0x0b, .meta = 0, .len = 2, .data = up_map_record},
                          {.type = 0x0b, .meta = 0, .len = 2, .data = down_map_record}};
    drv_queue_records(module, map, 2); // CHANNEL_MAP up/down relay pair

    uint8_t max_time_param[4], offset_param[4], feature_param[4];
    param(max_time_param, 0, wire_channel, roller_state->max_ms[channel_index]);
    param(offset_param, 1, wire_channel, roller_state->offset_ms[channel_index]);
    tfbus_tlv_t feature_records[2] = {{.type = 0x17, .meta = 0, .len = 4, .data = max_time_param},
                         {.type = 0x17, .meta = 0, .len = 4, .data = offset_param}};
    drv_queue_records(module, feature_records, 2);
    param(feature_param, 2, wire_channel, up_time(roller_state, channel_index));
    drv_queue_record(module, 0x17, 0x00, feature_param, 4);
    param(feature_param, 3, wire_channel, down_time(roller_state, channel_index));
    drv_queue_record(module, 0x17, 0x00, feature_param, 4);
    drv_queue_record(module, 0x04, 0x00, (uint8_t[]){0x16, (uint8_t)wire_channel, 0}, 3);
    drv_queue_record(module, 0x1c, 0x00, (uint8_t[]){0, (uint8_t)wire_channel}, 2);
}

/**
 * @brief tfbus_driver_t::init: full per-channel init (DIN_3) or CHANNEL_MAP pairs only (single channel).
 */
static void init(tfbus_module_t *module)
{
    roller_t *roller_state = ctx(module);
    for (int c = 0; c < roller_state->cfg->nch; c++) {
        if (roller_state->cfg->full_init) {
            init_channel(roller_state, c);
        } else {
            uint8_t up_map_record[2] = {(uint8_t)(2 * c + 1), 0}, down_map_record[2] = {(uint8_t)(2 * c + 2), 0};
            tfbus_tlv_t map[2] = {{.type = 0x0b, .len = 2, .data = up_map_record}, {.type = 0x0b, .len = 2, .data = down_map_record}};
            drv_queue_records(module, map, 2);
        }
    }
    roller_state->initialized = true;
}

/** @brief Stop channel @p channel_index (DEACTIVATE both directions) and cancel its stop timer. */
static void cmd_stop(roller_t *roller_state, int channel_index)
{
    roller_state->stop_timer[channel_index].gen++;
    roller_state->stop_timer[channel_index].due_us = 0;
    esp_timer_stop(roller_state->stop_timer[channel_index].timer);
    drv_queue_record(roller_state->m, 0x09, 0x01, (uint8_t[]){(uint8_t)(3 << (2 * channel_index))}, 1);
}

/**
 * @brief Timed move expired: stop the channel unless the timer was restarted / cancelled (@p gen).
 *
 * Bus task (deferred), under the driver lock.
 */
static void stop_timer_fire(tfbus_module_t *module, void *arg, uint32_t gen)
{
    chan_timer_t *timer_callback_state = arg;
    if (gen == timer_callback_state->gen && timer_callback_state->due_us) {
        cmd_stop(timer_callback_state->r, timer_callback_state->ch);
    }
}

/** @brief tfbus_driver_t::overdue: stop channels whose timed move expired but whose deferred stop was dropped. */
static void overdue(tfbus_module_t *module)
{
    roller_t *roller_state = module->drv_ctx;
    int64_t now = esp_timer_get_time();
    for (int c = 0; c < CH; c++) {
        if (roller_state->stop_timer[c].due_us && now >= roller_state->stop_timer[c].due_us) {
            cmd_stop(roller_state, c);
        }
    }
}

/** @brief esp_timer task: hand the stop over to the bus task (no blocking queue send here). */
static void stop_timer_cb(void *arg)
{
    chan_timer_t *timer_callback_state = arg;
    tfbus_master_defer(stop_timer_fire, timer_callback_state->r->m, timer_callback_state, timer_callback_state->gen);
}

/** @brief tfbus_driver_t::deinit: stop and delete the stop timers. */
static void deinit(tfbus_module_t *module)
{
    roller_t *roller_state = module->drv_ctx;
    for (int c = 0; c < CH; c++) {
        if (roller_state->stop_timer[c].timer) {
            esp_timer_stop(roller_state->stop_timer[c].timer);
            esp_timer_delete(roller_state->stop_timer[c].timer);
            roller_state->stop_timer[c].timer = NULL;
        }
    }
}

/**
 * @brief Move channel @p channel_index in @p dir (ST_UP / ST_DOWN); stop after @p time_ms if > 0. Ignored while blocked.
 */
static void cmd_move(roller_t *roller_state, int channel_index, int dir, uint32_t time_ms)
{
    if (roller_state->blocked[channel_index]) {
        return;
    }
    roller_state->active_ch = channel_index;
    roller_state->last_dir[channel_index] = dir;
    uint8_t bit = (uint8_t)(1 << (2 * channel_index + (dir == ST_DOWN ? 1 : 0)));
    drv_queue_record(roller_state->m, 0x08, 0x01, &bit, 1);
    roller_state->stop_timer[channel_index].gen++;
    roller_state->stop_timer[channel_index].due_us = 0;
    esp_timer_stop(roller_state->stop_timer[channel_index].timer);
    if (time_ms > 0 && roller_state->stop_timer[channel_index].timer) {
        roller_state->stop_timer[channel_index].due_us = esp_timer_get_time() + (int64_t)time_ms * 1000;
        esp_timer_start_once(roller_state->stop_timer[channel_index].timer, (uint64_t)time_ms * 1000);
    }
}

/**
 * @brief SET_POSITION channel @p channel_index to @p pct percent (clamped 0..100). Ignored while blocked.
 */
static void cmd_position(roller_t *roller_state, int channel_index, int pct)
{
    if (roller_state->blocked[channel_index]) {
        return;
    }
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    roller_state->active_ch = channel_index;
    drv_queue_record(roller_state->m, 0x16, 0x00, (uint8_t[]){0, (uint8_t)(channel_index + 1), (uint8_t)pct}, 3);
}

/** @brief tfbus_driver_t::io_new: remember the Lua name of a ROLLER_SHUTTER channel and create its stop timer. */
static void io_new(tfbus_module_t *module, int cls, int channel_index, const char *name)
{
    roller_t *roller_state = ctx(module);
    if (cls != CLS_ROLLER || channel_index < 0 || channel_index >= roller_state->cfg->nch) {
        return;
    }
    strlcpy(roller_state->name[channel_index], name, sizeof roller_state->name[channel_index]);
    if (roller_state->stop_timer[channel_index].timer) {
        return; // channel declared again: keep the existing timer (no leak)
    }
    roller_state->stop_timer[channel_index].r = roller_state;
    roller_state->stop_timer[channel_index].ch = channel_index;
    const esp_timer_create_args_t timer_args = {.callback = stop_timer_cb, .arg = &roller_state->stop_timer[channel_index], .name = "rol"};
    if (esp_timer_create(&timer_args, &roller_state->stop_timer[channel_index].timer) != ESP_OK) {
        roller_state->stop_timer[channel_index].timer = NULL;
    }
}

/**
 * @brief tfbus_driver_t::io_set: cache timing features (MaxTime, Up/DownMax, ...) and reconfigure if initialised.
 */
static bool io_set(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel_index, int idx, int val)
{
    roller_t *roller_state = ctx(module);
    if (cls != CLS_ROLLER || channel_index < 0 || channel_index >= roller_state->cfg->nch) {
        return false;
    }
    uint32_t configured_value = (uint32_t)luaL_optinteger(L, val, 0);
    if (idx == roller_state->cfg->maxtime_idx) {
        roller_state->max_ms[channel_index] = configured_value;
        if (roller_state->initialized && roller_state->cfg->full_init) {
            reconfigure_channel(roller_state, channel_index);
        }
        return false;
    }
    if (!roller_state->cfg->full_init) {
        return false;
    }
    switch (idx) {
    case F_UPMAX: roller_state->up_ms[channel_index] = configured_value; break;
    case F_DOWNMAX: roller_state->down_ms[channel_index] = configured_value; break;
    case F_OFFSET: roller_state->offset_ms[channel_index] = configured_value; break;
    default: return false;
    }
    if (roller_state->initialized) {
        reconfigure_channel(roller_state, channel_index);
    }
    return false; // keep generic storage so get() returns it
}

/**
 * @brief tfbus_driver_t::io_execute: MoveUp/MoveDown/Start/Stop/Hold*, SetPosition, Calibration, SetRollerBlocked.
 */
static int io_execute(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel_index, int idx, int first,
                      int nargs)
{
    roller_t *roller_state = ctx(module);
    if (cls != CLS_ROLLER || channel_index < 0 || channel_index >= roller_state->cfg->nch) {
        return -1;
    }
    uint32_t duration_ms = nargs > 0 ? (uint32_t)luaL_optinteger(L, first, 0) : 0;
    bool moving = roller_state->state[channel_index] == ST_UP || roller_state->state[channel_index] == ST_DOWN;
    switch (idx) {
    case 0: // MoveUp (or stop if moving)
        moving ? cmd_stop(roller_state, channel_index) : cmd_move(roller_state, channel_index, ST_UP, duration_ms);
        return 0;
    case 1: // MoveDown (or stop if moving)
        moving ? cmd_stop(roller_state, channel_index) : cmd_move(roller_state, channel_index, ST_DOWN, duration_ms);
        return 0;
    case 2: // Start: reverse of the previous direction
        moving ? cmd_stop(roller_state, channel_index) : cmd_move(roller_state, channel_index, roller_state->last_dir[channel_index] == ST_UP ? ST_DOWN : ST_UP, duration_ms);
        return 0;
    case 3: // Stop
        cmd_stop(roller_state, channel_index);
        return 0;
    case 4: // Hold (direction change)
        cmd_move(roller_state, channel_index, roller_state->last_dir[channel_index] == ST_UP ? ST_DOWN : ST_UP, 0);
        return 0;
    case 5: cmd_move(roller_state, channel_index, ST_UP, 0); return 0;   // HoldUp
    case 6: cmd_move(roller_state, channel_index, ST_DOWN, 0); return 0; // HoldDown
    case 10: // SetPosition(pos%)
        cmd_position(roller_state, channel_index, nargs > 0 ? (int)luaL_checkinteger(L, first) : 0);
        return 0;
    case 13: // SetRollerBlocked
        roller_state->blocked[channel_index] = nargs > 0 && luaL_optinteger(L, first, 0) != 0;
        if (roller_state->blocked[channel_index]) {
            cmd_stop(roller_state, channel_index);
        }
        clu_lua_post_number(roller_state->name[channel_index], F_STATE, roller_state->blocked[channel_index] ? ST_BLOCKED : ST_STOP);
        return 0;
    case 8: // Calibration(dir): drive fully in that direction for the full travel time
        cmd_move(roller_state, channel_index, nargs > 0 && luaL_optinteger(L, first, 1) ? ST_UP : ST_DOWN,
                 roller_state->max_ms[channel_index] + roller_state->max_ms[channel_index] / 10);
        return 0;
    default:
        ESP_LOGW(TAG, "%s: execute(%d) not implemented", roller_state->name[channel_index], idx);
        return 0;
    }
}

/** @brief Wire state byte -> logical state (1/0x10 up, 2/0x20 down, else stop). */
static int decode_state(uint8_t wire_state)
{
    switch (wire_state) {
    case 0x01: case 0x10: return ST_UP;
    case 0x02: case 0x20: return ST_DOWN;
    default: return ST_STOP;
    }
}

/**
 * @brief Report a channel state change to Lua (State/Up/Down values; state-change, start, up/down and stop events).
 */
static void publish_state(roller_t *roller_state, int channel_index, int logical_state)
{
    if (roller_state->state[channel_index] == logical_state || !roller_state->name[channel_index][0]) {
        roller_state->state[channel_index] = logical_state;
        return;
    }
    int prev = roller_state->state[channel_index];
    roller_state->state[channel_index] = logical_state;
    const char *object_name = roller_state->name[channel_index];
    clu_lua_post_number(object_name, F_STATE, logical_state);
    clu_lua_post_number(object_name, F_UP, logical_state == ST_UP);
    clu_lua_post_number(object_name, F_DOWN, logical_state == ST_DOWN);
    clu_lua_post_event(object_name, EV_STATE);
    if (logical_state == ST_UP || logical_state == ST_DOWN) {
        if (prev == ST_STOP) {
            clu_lua_post_event(object_name, EV_START);
        }
        clu_lua_post_event(object_name, logical_state == ST_UP ? EV_UP : EV_DOWN);
    } else if (logical_state == ST_STOP) {
        clu_lua_post_event(object_name, EV_STOP);
    }
}

/**
 * @brief tfbus_driver_t::on_status: module-wide state byte and per-channel position records.
 */
static void on_status(tfbus_module_t *module, const uint8_t *status_payload, size_t len)
{
    roller_t *roller_state = ctx(module);
    tfbus_tlv_t rec[6];
    int record_count = tfbus_parse_tlv(status_payload, len, rec, 6);
    int mod_state = ST_STOP;
    for (int i = 0; i < record_count; i++) {
        if (rec[i].type == 0x01 && rec[i].len >= 1) {
            mod_state = decode_state(rec[i].data[0]);
        } else if (rec[i].type == 0x16 && rec[i].len >= 3) {
            int channel_index = rec[i].data[0] - 1;
            int pos = rec[i].data[1];
            if (channel_index < 0 || channel_index >= roller_state->cfg->nch || !roller_state->name[channel_index][0]) {
                continue;
            }
            if (pos != roller_state->pos[channel_index]) {
                bool first = roller_state->pos[channel_index] < 0;
                roller_state->pos[channel_index] = pos;
                clu_lua_post_number(roller_state->name[channel_index], F_POSITION, pos);
                if (!first) {
                    clu_lua_post_event(roller_state->name[channel_index], EV_POSITION);
                }
            }
        }
    }
    // The state byte is module-wide: attribute it to the channel we last commanded.
    for (int c = 0; c < roller_state->cfg->nch; c++) {
        if (roller_state->state[c] == ST_BLOCKED || roller_state->blocked[c]) {
            continue;
        }
        publish_state(roller_state, c, c == roller_state->active_ch ? mod_state : ST_STOP);
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
