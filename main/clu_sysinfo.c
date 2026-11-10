/**
 * @file clu_sysinfo.c
 * @brief Firmware internals for Lua (merged into the private __clu table, used by mqtt.lua).
 *
 * - `__clu.sys_info()`       -> {heap_free, heap_min, psram_free, rssi?, ip?, reset_reason,
 *                                tfbus_total, tfbus_online, tfbus_fails}
 * - `__clu.tfbus_module(sn)` -> {online, fails, polls, addr, hw_type} | nil
 * - `__clu.sys_restart()`    -> reboots ~1 s later (lets the MQTT/UDP reply go out first)
 */
#include "clu_sysinfo.h"

#include "clu_lua.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "tfbus_master.h"

static const char *TAG = "sysinfo";

/** @brief Last reset reason as a short string. */
static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
    }
}

/** @brief Set integer field @p k of the table on top of the stack. */
static void set_int(lua_State *L, const char *k, lua_Integer v)
{
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
}

/** @brief Lua: `__clu.sys_info()` — heap, WiFi, reset reason and TFBUS totals as a table. */
static int l_sys_info(lua_State *L)
{
    lua_newtable(L);
    set_int(L, "heap_free", (lua_Integer)esp_get_free_heap_size());
    set_int(L, "heap_min", (lua_Integer)esp_get_minimum_free_heap_size());
    set_int(L, "psram_free", (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    lua_pushstring(L, reset_reason());
    lua_setfield(L, -2, "reset_reason");

    int rssi;
    if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
        set_int(L, "rssi", rssi);
    }
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr) {
        char buf[16];
        esp_ip4addr_ntoa(&ip.ip, buf, sizeof buf);
        lua_pushstring(L, buf);
        lua_setfield(L, -2, "ip");
    }

    // Registry is fixed after OM.LUA; online/fails are single words written by the bus task.
    int n = tfbus_master_count(), online = 0;
    lua_Integer fails = 0;
    for (int i = 0; i < n; i++) {
        const tfbus_module_t *m = tfbus_master_get(i);
        online += m->online;
        fails += m->fails_total;
    }
    set_int(L, "tfbus_total", n);
    set_int(L, "tfbus_online", online);
    set_int(L, "tfbus_fails", fails);
    return 1;
}

/** @brief Lua: `__clu.tfbus_module(sn)` — stats of one module, nil if not declared. */
static int l_tfbus_module(lua_State *L)
{
    const tfbus_module_t *m = tfbus_master_find_sn((uint32_t)luaL_checkinteger(L, 1));
    if (!m) {
        return 0;
    }
    lua_newtable(L);
    lua_pushboolean(L, m->online);
    lua_setfield(L, -2, "online");
    set_int(L, "fails", m->fails_total);
    set_int(L, "polls", m->polls);
    set_int(L, "addr", m->addr);
    set_int(L, "hw_type", m->hw_type);
    return 1;
}

/** @brief esp_timer callback: reboot. */
static void do_restart(void *arg)
{
    esp_restart();
}

/** @brief Lua: `__clu.sys_restart()` — run the close hooks and reboot after 1 s. */
static int l_sys_restart(lua_State *L)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = {.callback = do_restart, .name = "restart"};
        if (esp_timer_create(&a, &t) != ESP_OK) {
            return 0;
        }
    }
    ESP_LOGW(TAG, "restart requested from Lua/MQTT");
    clu_lua_shutdown_hooks(L); // pending storage writes
    esp_timer_start_once(t, 1000 * 1000); // fails harmlessly if already pending
    return 0;
}

static const luaL_Reg s_fns[] = {
    {"sys_info", l_sys_info},
    {"tfbus_module", l_tfbus_module},
    {"sys_restart", l_sys_restart},
    {NULL, NULL},
};

void clu_sysinfo_init(void)
{
    clu_lua_register_ext(s_fns, NULL, 0, "=sysinfo");
}
