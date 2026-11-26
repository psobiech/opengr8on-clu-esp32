/**
 * @file drv_psu.c
 * @brief PowerSupplyVoltage (class 12) shared by the module drivers.
 */
#include "drv_psu.h"

#include <stdlib.h>

#include "clu_lua.h"
#include "lauxlib.h"
#include "tfbus_drivers.h"

/** @brief PowerSupplyVoltage features. */
enum { P_VALUE = 0, P_PERCENT = 1, P_SENS = 3, P_MIN = 5, P_MAX = 6 };
/** @brief PowerSupplyVoltage events. */
enum { PE_CHANGE = 0, PE_LOWER = 1, PE_RISE = 2, PE_OUT = 3, PE_IN = 6 };

void psu_init(psu_t *psu)
{
    psu->sensitivity = 50; // 0.5 V
    psu->min = 2100;       // 21 V
    psu->max = 2400;       // 24 V
}

bool psu_io_set(psu_t *psu, lua_State *L, int idx, int val)
{
    int32_t voltage = drv_centi(luaL_checknumber(L, val));
    if (idx == P_SENS) {
        psu->sensitivity = voltage;
    } else if (idx == P_MIN) {
        psu->min = voltage;
    } else if (idx == P_MAX) {
        psu->max = voltage;
    }
    return false;
}

void psu_update(psu_t *psu, int32_t voltage)
{
    const char *object_name = psu->name;
    bool first = !psu->have_voltage;
    psu->have_voltage = true;
    psu->voltage = voltage;
    bool out = voltage < psu->min || voltage > psu->max;
    bool change = first || abs(voltage - psu->last_event_voltage) >= psu->sensitivity;
    if (object_name[0] && (change || out != psu->out_of_range)) {
        clu_lua_post_number(object_name, P_VALUE, (float)voltage / 100.0f);
        // Value% = U / MaxValue * 100, not rounded (real CLU: 23.13 V of 24 V -> 96.375)
        float percent = psu->max > 0 ? (float)voltage * 100.0f / (float)psu->max : 0.0f;
        clu_lua_post_number(object_name, P_PERCENT, percent);
    }
    if (object_name[0] && change && !first) {
        clu_lua_post_event(object_name, PE_CHANGE);
        clu_lua_post_event(object_name, voltage < psu->last_event_voltage ? PE_LOWER : PE_RISE);
    }
    if (change) {
        psu->last_event_voltage = voltage;
    }
    if (object_name[0] && out != psu->out_of_range && !(first && !out)) {
        clu_lua_post_event(object_name, out ? PE_OUT : PE_IN);
    }
    psu->out_of_range = out;
}
