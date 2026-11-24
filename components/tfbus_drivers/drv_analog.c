/**
 * @file drv_analog.c
 * @brief ANALOG_DIN (hwType 25 / 0x19) and ONE_WIRE sensors (hwType 255 / 0xff), as emulated by
 * the firmware for on-board analog inputs and 1-Wire sensors (wire format in tfbus_vmod.h; the real
 * modules' frames are not known, so a real ANALOG_DIN on RS485 is not driven: this driver sends it
 * nothing but the REFRESH record of RefreshSensors).
 *
 * Interfaces module_2_0_ANALOG_DIN_fv02_02.xml and module_one_wire_fv03_28.xml / fv01_10.xml:
 * - AnalogIN (class 12, 7x): Value (0) = RawValue x Scale, Value% (1) of MinValue..MaxValue,
 *   Scale (2), Sensitivity (3, change of RawValue in V that fires the change events), RawValue (4),
 *   MinValue (5) / MaxValue (6) of Value. Events OnValueChange 0, OnValueLower 1, OnValueRise 2,
 *   OnOutOfRange 3, OnSwitchOn 4 (Value reaches MaxValue), OnSwitchOff 5 (reaches MinValue),
 *   OnInRange 6.
 * - AnalogOUT (class 13, 4x): no hardware; features stored as set, methods ignored.
 * - ONE_WIRE_HUB (class 73): ConfiguredSensors (0, its sensors declared in OM.LUA), ActiveSensors
 *   (1, of those connected), InactiveSensors (2), ReportedSensors (3, found by the last search);
 *   RefreshSensors (0) searches again, OnRefreshCompleted (0) when done.
 * - ONEW_SENSOR (class 23): Value (0) [°C], Threshold (1), MinValue (2), MaxValue (3), Status (4:
 *   0 disconnected, 1 connected, 2 reset state, 3 module unreachable), HubSerialNumber (5),
 *   SensorSerialNumber (6, the 48-bit serial of the ROM). Events OnValueChange 0, OnValueRise 1,
 *   OnValueLower 2, OnOutOfRange 3, OnConnect 4, OnDisconnect 5.
 */
#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "drv_panel_common.h"
#include "lauxlib.h"
#include "tfbus_drivers.h"
#include "tfbus_master.h"
#include "tfbus_vmod.h"

/** @brief OM object classes. */
enum { CLS_ANALOG_IN = 12, CLS_ANALOG_OUT = 13, CLS_ONEW_SENSOR = 23, CLS_ONE_WIRE_HUB = 73 };

/** @brief AnalogIN events. */
enum {
    AIN_EV_CHANGE = 0, AIN_EV_LOWER = 1, AIN_EV_RISE = 2, AIN_EV_OUT_OF_RANGE = 3,
    AIN_EV_SWITCH_ON = 4, AIN_EV_SWITCH_OFF = 5, AIN_EV_IN_RANGE = 6,
};

/** @brief ONEW_SENSOR Status values beyond the wire states, and its connection events. */
enum { SENSOR_STATUS_UNREACHABLE = 3 };
enum { SENSOR_EV_CONNECT = 4, SENSOR_EV_DISCONNECT = 5 };

/** @brief One AnalogIN; values in 1/1000 (V, or the scaled unit for Value / Min / Max). */
typedef struct {
    char name[24];
    int32_t scale, sensitivity, min_value, max_value;
    int32_t raw_value, last_event_raw_value;
    bool have_value, out_of_range, at_max, at_min;
} analog_input_t;

/** @brief ANALOG_DIN driver state (drv_ctx). */
typedef struct {
    analog_input_t inputs[TFBUS_ANALOG_INPUTS];
    char hub_name[24];
    uint8_t found_sensors, searches_done;
    bool have_hub_status;
} analog_state_t;

/** @brief ONE_WIRE driver state (drv_ctx). */
typedef struct {
    panel_sensor_t sensor;   // value, threshold / range events
    uint8_t status;          // Status feature
    bool have_status;
    uint32_t hub_serial_number;
    uint64_t rom_code;
} onewire_state_t;

static void analog_io_new(tfbus_module_t *module, int cls, int channel, const char *name);
static bool analog_io_set(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel, int idx, int val);
static int analog_io_execute(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel, int idx, int first,
                             int nargs);
static bool analog_io_get(tfbus_module_t *module, lua_State *L, int cls, int channel, int idx);
static void analog_on_status(tfbus_module_t *module, const uint8_t *payload, size_t len);
static void onewire_io_new(tfbus_module_t *module, int cls, int channel, const char *name);
static bool onewire_io_set(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel, int idx, int val);
static bool onewire_io_get(tfbus_module_t *module, lua_State *L, int cls, int channel, int idx);
static void onewire_on_status(tfbus_module_t *module, const uint8_t *payload, size_t len);
static void onewire_on_online(tfbus_module_t *module, bool online);

const tfbus_driver_t tfbus_drv_analog_din = {
    .hw_type = TFBUS_ANALOG_HW_TYPE,
    .name = "ANALOG_DIN",
    .on_status = analog_on_status,
    .io_new = analog_io_new,
    .io_set = analog_io_set,
    .io_execute = analog_io_execute,
    .io_get = analog_io_get,
};

const tfbus_driver_t tfbus_drv_one_wire = {
    .hw_type = TFBUS_ONEWIRE_HW_TYPE,
    .name = "ONE_WIRE",
    .on_status = onewire_on_status,
    .on_online = onewire_on_online,
    .io_new = onewire_io_new,
    .io_set = onewire_io_set,
    .io_get = onewire_io_get,
};

// ---------------------------------------------------------------------------------------- ANALOG_DIN

/** @brief ANALOG_DIN state of @p module, allocated (with the interface defaults) on first use. */
static analog_state_t *analog_state(tfbus_module_t *module)
{
    if (!module->drv_ctx) {
        analog_state_t *state = calloc(1, sizeof *state);
        if (!state) {
            abort(); // out of memory while loading OM.LUA: reboot instead of a NULL dereference
        }
        for (int i = 0; i < TFBUS_ANALOG_INPUTS; i++) {
            state->inputs[i] = (analog_input_t){.scale = 1000, .sensitivity = 10, .max_value = 10000};
        }
        module->drv_ctx = state;
    }
    return module->drv_ctx;
}

/** @brief tfbus_driver_t::io_new: remember the Lua names of the AnalogIN objects and the hub. */
static void analog_io_new(tfbus_module_t *module, int cls, int channel, const char *name)
{
    analog_state_t *state = analog_state(module);
    if (cls == CLS_ANALOG_IN && channel >= 0 && channel < TFBUS_ANALOG_INPUTS) {
        strlcpy(state->inputs[channel].name, name, sizeof state->inputs[channel].name);
    } else if (cls == CLS_ONE_WIRE_HUB && channel == 0) {
        strlcpy(state->hub_name, name, sizeof state->hub_name);
    }
}

static void analog_input_publish(analog_input_t *input, bool with_events);

/** @brief tfbus_driver_t::io_set: AnalogIN Scale (2), Sensitivity (3), MinValue (5), MaxValue (6) are cached. */
static bool analog_io_set(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel, int idx, int val)
{
    if (cls != CLS_ANALOG_IN || channel < 0 || channel >= TFBUS_ANALOG_INPUTS) {
        return false;
    }
    analog_input_t *input = &analog_state(module)->inputs[channel];
    int32_t milli_value = drv_milli((float)luaL_optnumber(L, val, 0));
    switch (idx) {
    case 2: input->scale = milli_value; break;
    case 3: input->sensitivity = milli_value < 0 ? -milli_value : milli_value; break;
    case 5: input->min_value = milli_value; break;
    case 6: input->max_value = milli_value; break;
    default: return false;
    }
    if (input->have_value) {
        analog_input_publish(input, false); // Value / Value% follow the new scale / range
    }
    return false; // store generically
}

/** @brief tfbus_driver_t::io_execute: ONE_WIRE_HUB RefreshSensors (0); the rest has nothing to do. */
static int analog_io_execute(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel, int idx, int first,
                             int nargs)
{
    if (cls == CLS_ONE_WIRE_HUB && idx == 0) {
        drv_queue_record(module, TFBUS_ANALOG_REC_REFRESH, 0, NULL, 0);
        return 0;
    }
    return -1;
}

static int hub_sensor_count(const tfbus_module_t *hub, bool connected_only);

/** @brief tfbus_driver_t::io_get: the ONE_WIRE_HUB sensor counts. */
static bool analog_io_get(tfbus_module_t *module, lua_State *L, int cls, int channel, int idx)
{
    if (cls != CLS_ONE_WIRE_HUB) {
        return false;
    }
    int configured_sensors = hub_sensor_count(module, false);
    int active_sensors = hub_sensor_count(module, true);
    switch (idx) {
    case 0: lua_pushinteger(L, configured_sensors); return true;
    case 1: lua_pushinteger(L, active_sensors); return true;
    case 2: lua_pushinteger(L, configured_sensors - active_sensors); return true;
    case 3: lua_pushinteger(L, analog_state(module)->found_sensors); return true;
    default: return false;
    }
}

/**
 * @brief Sensors of @p hub declared in OM.LUA (ONE_WIRE modules whose last status names it), or
 *        only the connected ones. Lua task, driver lock held: the registry changes only while
 *        OM.LUA loads, in this task.
 */
static int hub_sensor_count(const tfbus_module_t *hub, bool connected_only)
{
    int sensor_count = 0;
    for (int i = 0; i < tfbus_master_count(); i++) {
        const tfbus_module_t *module = tfbus_master_get(i);
        if (!module || module->drv != &tfbus_drv_one_wire || !module->name[0] || !module->drv_ctx) {
            continue;
        }
        const onewire_state_t *sensor_state = module->drv_ctx;
        if (sensor_state->have_status && sensor_state->hub_serial_number == hub->sn &&
            (!connected_only || (module->online && sensor_state->status == TFBUS_ONEWIRE_CONNECTED))) {
            sensor_count++;
        }
    }
    return sensor_count;
}

/** @brief tfbus_driver_t::on_status: input values (and their events), 1-Wire search results. */
static void analog_on_status(tfbus_module_t *module, const uint8_t *payload, size_t len)
{
    analog_state_t *state = analog_state(module);
    tfbus_tlv_t records[4];
    int record_count = tfbus_parse_tlv(payload, len, records, 4);
    for (int i = 0; i < record_count && i < 4; i++) {
        const tfbus_tlv_t *record = &records[i];
        if (record->type == TFBUS_ANALOG_REC_INPUTS && record->len >= 2 * TFBUS_ANALOG_INPUTS) {
            for (int channel = 0; channel < TFBUS_ANALOG_INPUTS; channel++) {
                analog_input_t *input = &state->inputs[channel];
                int32_t raw_value = record->data[2 * channel] | record->data[2 * channel + 1] << 8;
                if (!input->name[0] || (input->have_value && raw_value == input->raw_value)) {
                    continue;
                }
                bool with_events = input->have_value;
                input->raw_value = raw_value;
                input->have_value = true;
                analog_input_publish(input, with_events);
            }
        } else if (record->type == TFBUS_ANALOG_REC_HUB && record->len >= 2) {
            bool search_finished = state->have_hub_status && record->data[1] != state->searches_done;
            state->found_sensors = record->data[0];
            state->searches_done = record->data[1];
            state->have_hub_status = true;
            if (search_finished && state->hub_name[0]) {
                clu_lua_post_event(state->hub_name, 0); // OnRefreshCompleted
            }
        }
    }
}

/**
 * @brief Post an input's Value, Value% and RawValue; with @p with_events also its change, range and
 *        switch events. The first value (and a new scale / range) only sets the event state.
 */
static void analog_input_publish(analog_input_t *input, bool with_events)
{
    int32_t scaled_value = (int32_t)((int64_t)input->raw_value * input->scale / 1000);
    int32_t span = input->max_value - input->min_value;
    float percent = span ? (float)(scaled_value - input->min_value) * 100.0f / (float)span : 0.0f;
    clu_lua_post_number(input->name, 0, (float)scaled_value / 1000.0f);
    clu_lua_post_number(input->name, 1, percent);
    clu_lua_post_number(input->name, 4, (float)input->raw_value / 1000.0f);

    bool out_of_range = scaled_value < input->min_value || scaled_value > input->max_value;
    bool at_max = scaled_value >= input->max_value, at_min = scaled_value <= input->min_value;
    int32_t change = input->raw_value - input->last_event_raw_value;
    if (!with_events) {
        input->last_event_raw_value = input->raw_value;
    } else {
        if (abs(change) >= input->sensitivity && change != 0) {
            input->last_event_raw_value = input->raw_value;
            clu_lua_post_event(input->name, AIN_EV_CHANGE);
            clu_lua_post_event(input->name, change < 0 ? AIN_EV_LOWER : AIN_EV_RISE);
        }
        if (out_of_range != input->out_of_range) {
            clu_lua_post_event(input->name, out_of_range ? AIN_EV_OUT_OF_RANGE : AIN_EV_IN_RANGE);
        }
        if (at_max && !input->at_max) {
            clu_lua_post_event(input->name, AIN_EV_SWITCH_ON);
        }
        if (at_min && !input->at_min) {
            clu_lua_post_event(input->name, AIN_EV_SWITCH_OFF);
        }
    }
    input->out_of_range = out_of_range;
    input->at_max = at_max;
    input->at_min = at_min;
}

// ---------------------------------------------------------------------------------------- ONE_WIRE

/** @brief ONE_WIRE state of @p module, allocated (with the DS18B20 interface defaults) on first use. */
static onewire_state_t *onewire_state(tfbus_module_t *module)
{
    if (!module->drv_ctx) {
        onewire_state_t *state = calloc(1, sizeof *state);
        if (!state) {
            abort(); // out of memory while loading OM.LUA: reboot instead of a NULL dereference
        }
        state->sensor = (panel_sensor_t){.threshold = 50, .min = -5500, .max = 12500};
        module->drv_ctx = state;
    }
    return module->drv_ctx;
}

/** @brief tfbus_driver_t::io_new: remember the Lua name of the ONEW_SENSOR. */
static void onewire_io_new(tfbus_module_t *module, int cls, int channel, const char *name)
{
    if (cls == CLS_ONEW_SENSOR && channel == 0) {
        onewire_state_t *state = onewire_state(module);
        strlcpy(state->sensor.name, name, sizeof state->sensor.name);
    }
}

/** @brief tfbus_driver_t::io_set: Threshold (1), MinValue (2), MaxValue (3) are cached [1/100 °C]. */
static bool onewire_io_set(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel, int idx, int val)
{
    if (cls != CLS_ONEW_SENSOR) {
        return false;
    }
    panel_sensor_t *sensor = &onewire_state(module)->sensor;
    int32_t centi_value = drv_centi((float)luaL_optnumber(L, val, 0));
    switch (idx) {
    case 1: sensor->threshold = centi_value; break;
    case 2: sensor->min = centi_value; break;
    case 3: sensor->max = centi_value; break;
    default: break;
    }
    return false; // store generically
}

/** @brief tfbus_driver_t::io_get: HubSerialNumber (5), SensorSerialNumber (6) as exact integers. */
static bool onewire_io_get(tfbus_module_t *module, lua_State *L, int cls, int channel, int idx)
{
    const onewire_state_t *state = module->drv_ctx;
    if (cls != CLS_ONEW_SENSOR || !state || !state->have_status) {
        return false;
    }
    if (idx == 5) {
        lua_pushinteger(L, (lua_Integer)state->hub_serial_number);
        return true;
    }
    if (idx == 6) {
        lua_pushinteger(L, (lua_Integer)((state->rom_code >> 8) & 0xFFFFFFFFFFFFULL));
        return true;
    }
    return false;
}

static void onewire_set_status(onewire_state_t *state, uint8_t status);

/** @brief tfbus_driver_t::on_status: Status (and connect events), temperature with its events. */
static void onewire_on_status(tfbus_module_t *module, const uint8_t *payload, size_t len)
{
    onewire_state_t *state = onewire_state(module);
    tfbus_tlv_t records[2];
    int record_count = tfbus_parse_tlv(payload, len, records, 2);
    for (int i = 0; i < record_count && i < 2; i++) {
        const uint8_t *data = records[i].data;
        if (records[i].type != TFBUS_ONEWIRE_REC_SENSOR || records[i].len < TFBUS_ONEWIRE_STATUS_LEN - 4) {
            continue;
        }
        state->hub_serial_number = data[3] | data[4] << 8 | data[5] << 16 | (uint32_t)data[6] << 24;
        state->rom_code = 0;
        for (int byte_index = 0; byte_index < 8; byte_index++) {
            state->rom_code |= (uint64_t)data[7 + byte_index] << (8 * byte_index);
        }
        onewire_set_status(state, data[0]);
        if (data[0] == TFBUS_ONEWIRE_CONNECTED) {
            panel_sensor_update(&state->sensor, (int16_t)(data[1] | data[2] << 8));
        }
    }
}

/** @brief tfbus_driver_t::on_online: an unreachable module is Status 3 (and disconnected). */
static void onewire_on_online(tfbus_module_t *module, bool online)
{
    onewire_state_t *state = onewire_state(module);
    if (!online && state->have_status) {
        onewire_set_status(state, SENSOR_STATUS_UNREACHABLE);
    }
}

/**
 * @brief Post a Status change. OnConnect when a sensor comes back (not for the first reading after
 *        start), OnDisconnect when a connected one stops answering.
 */
static void onewire_set_status(onewire_state_t *state, uint8_t status)
{
    if (state->have_status && status == state->status) {
        return;
    }
    const char *name = state->sensor.name;
    if (name[0]) {
        clu_lua_post_number(name, 4, status);
        if (state->have_status && status == TFBUS_ONEWIRE_CONNECTED &&
            (state->status == TFBUS_ONEWIRE_DISCONNECTED || state->status == SENSOR_STATUS_UNREACHABLE)) {
            clu_lua_post_event(name, SENSOR_EV_CONNECT);
        } else if (state->have_status && state->status == TFBUS_ONEWIRE_CONNECTED) {
            clu_lua_post_event(name, SENSOR_EV_DISCONNECT);
        }
    }
    state->status = status;
    state->have_status = true;
}
