/**
 * @file clu_sysinfo.c
 * @brief Firmware internals for Lua (merged into the private __clu table, used by mqtt.lua).
 *
 * - `__clu.sys_info()`       -> {heap_free, heap_min, psram_free, rssi? (WiFi), ip? (Ethernet first,
 *                                else WiFi), reset_reason, last_crash? (clu_crash),
 *                                tfbus_total, tfbus_online, tfbus_fails, tfbus_load?, tfbus_busy?}; tfbus_load /
 *                                tfbus_busy: % of the last 10 s window with bytes on the wire / the bus
 *                                held by the master (tfbus_uart_load()), absent without a wire;
 *                                tfbus_late_replies?, tfbus_rx_overflows?, tfbus_rx_frame_errors?,
 *                                tfbus_rx_breaks?: receive problems since boot (tfbus_uart_stats()),
 *                                absent without a wire; cpu_load_0?, cpu_load_1?: % of time each core
 *                                was not idle, averaged since the previous window (>= 10 s, see
 *                                update_cpu_load()), absent without CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
 * - `__clu.tfbus_module(sn)` -> {online, fails, polls, addr, hw_type, fw_api_ver, hw_ver?, fw_ver?} | nil;
 *                                hw_ver / fw_ver ("a.b.c") once VERSION_QUERY answered
 * - `__clu.local_channels(sn)` -> {[OM class] = bit mask of the channels with hardware behind them}
 *                                for an on-board module (clu_localio), nil for any other; classes left
 *                                out have none
 * - `__clu.sys_restart()`    -> reboots ~1 s later (lets the MQTT/UDP reply go out first)
 */
#include "clu_sysinfo.h"

#include "clu_crash.h"
#include "clu_localio.h"
#include "clu_lua.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tfbus_master.h"
#include "tfbus_uart.h"
#if CONFIG_CLU_WIFI
#include "esp_wifi.h"
#endif

static const char *TAG = "sysinfo";

/** @brief OM object classes of the on-board module channels (clu_localio). */
enum { OM_CLASS_DIN = 3, OM_CLASS_DOUT = 4, OM_CLASS_ANALOG_IN = 12, OM_CLASS_ONE_WIRE_HUB = 73 };

static void set_class_channels(lua_State *L, int object_class, uint8_t channel_mask);

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
/** @brief Shortest CPU load window: sys_info() calls inside it return the previous window's load,
 *  so several readers (MQTT, HTTP, telnet) do not shrink each other's window. */
#define CPU_LOAD_MIN_WINDOW_US (10 * 1000 * 1000LL)
/** @brief Longest usable window: the 32-bit µs run-time counter wraps after ~71 min. */
#define CPU_LOAD_MAX_WINDOW_US (3600 * 1000 * 1000LL)

/** @brief Start of the current window (all zero = scheduler start, so the first window is since boot). */
static int64_t s_cpu_window_start_us;
static configRUN_TIME_COUNTER_TYPE s_cpu_window_start_counter;
static configRUN_TIME_COUNTER_TYPE s_cpu_window_start_idle[configNUMBER_OF_CORES];
/** @brief Busy share of each core in the last finished window, valid once one has finished. */
static uint16_t s_cpu_load_permille[configNUMBER_OF_CORES];
static bool s_cpu_load_valid;

static void update_cpu_load(void);
#endif

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

/** @brief Set integer field @p field_name of the table on top of the stack. */
static void set_int(lua_State *L, const char *field_name, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, field_name);
}

/** @brief Lua: `__clu.sys_info()` — heap, network, reset reason and TFBUS totals as a table. */
static int l_sys_info(lua_State *L)
{
    lua_newtable(L);
    set_int(L, "heap_free", (lua_Integer)esp_get_free_heap_size());
    set_int(L, "heap_min", (lua_Integer)esp_get_minimum_free_heap_size());
    set_int(L, "psram_free", (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    lua_pushstring(L, reset_reason());
    lua_setfield(L, -2, "reset_reason");
    const char *last_crash = clu_crash_last();
    if (last_crash) {
        lua_pushstring(L, last_crash);
        lua_setfield(L, -2, "last_crash");
    }

#if CONFIG_CLU_WIFI
    int rssi;
    if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
        set_int(L, "rssi", rssi);
    }
#endif
    static const char *const ifkeys[] = {"ETH_DEF", "WIFI_STA_DEF"}; // wired first
    for (size_t i = 0; i < sizeof ifkeys / sizeof ifkeys[0]; i++) {
        esp_netif_ip_info_t ip;
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey(ifkeys[i]);
        if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) {
            char buf[16];
            esp_ip4addr_ntoa(&ip.ip, buf, sizeof buf);
            lua_pushstring(L, buf);
            lua_setfield(L, -2, "ip");
            break;
        }
    }

    // online/fails are single words written by the bus task; freed slots (refresh) are NULL.
    int module_count = 0, online = 0;
    lua_Integer fails = 0;
    for (int i = 0; i < tfbus_master_count(); i++) {
        const tfbus_module_t *module = tfbus_master_get(i);
        if (module) {
            module_count++;
            online += module->online;
            fails += module->fails_total;
        }
    }
    set_int(L, "tfbus_total", module_count);
    set_int(L, "tfbus_online", online);
    set_int(L, "tfbus_fails", fails);
    tfbus_uart_load_t bus_load;
    if (tfbus_uart_load(&bus_load)) {
        lua_pushnumber(L, bus_load.wire_permille / 10.0);
        lua_setfield(L, -2, "tfbus_load");
        lua_pushnumber(L, bus_load.busy_permille / 10.0);
        lua_setfield(L, -2, "tfbus_busy");
    }
    tfbus_uart_stats_t bus_stats;
    if (tfbus_uart_stats(&bus_stats)) {
        set_int(L, "tfbus_late_replies", bus_stats.late_replies);
        set_int(L, "tfbus_rx_overflows", bus_stats.rx_overflows);
        set_int(L, "tfbus_rx_frame_errors", bus_stats.frame_errors);
        set_int(L, "tfbus_rx_breaks", bus_stats.breaks);
    }
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    update_cpu_load();
    if (s_cpu_load_valid) {
        char field_name[] = "cpu_load_0";
        for (int core = 0; core < configNUMBER_OF_CORES; core++) {
            field_name[sizeof field_name - 2] = (char)('0' + core);
            lua_pushnumber(L, s_cpu_load_permille[core] / 10.0);
            lua_setfield(L, -2, field_name);
        }
    }
#endif
    return 1;
}

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
/**
 * @brief Close the CPU load window once it is CPU_LOAD_MIN_WINDOW_US long: per core, busy =
 *        1 - idle task run time / window run time. A window longer than CPU_LOAD_MAX_WINDOW_US
 *        (the counter may have wrapped) only starts a new one.
 */
static void update_cpu_load(void)
{
    int64_t current_time_us = esp_timer_get_time();
    int64_t window_us = current_time_us - s_cpu_window_start_us;
    if (window_us < CPU_LOAD_MIN_WINDOW_US) {
        return;
    }
    configRUN_TIME_COUNTER_TYPE current_counter = portGET_RUN_TIME_COUNTER_VALUE();
    configRUN_TIME_COUNTER_TYPE window_counter = current_counter - s_cpu_window_start_counter;
    bool window_usable = window_us <= CPU_LOAD_MAX_WINDOW_US && window_counter > 0;
    for (int core = 0; core < configNUMBER_OF_CORES; core++) {
        configRUN_TIME_COUNTER_TYPE idle_counter = ulTaskGetIdleRunTimeCounterForCore(core);
        configRUN_TIME_COUNTER_TYPE window_idle = idle_counter - s_cpu_window_start_idle[core];
        if (window_usable) {
            // The idle slice running now is counted only when it ends: idle can exceed the window.
            uint64_t idle_permille = (uint64_t)window_idle * 1000 / window_counter;
            s_cpu_load_permille[core] = idle_permille >= 1000 ? 0 : (uint16_t)(1000 - idle_permille);
        }
        s_cpu_window_start_idle[core] = idle_counter;
    }
    s_cpu_window_start_us = current_time_us;
    s_cpu_window_start_counter = current_counter;
    s_cpu_load_valid |= window_usable;
}
#endif

/** @brief Lua: `__clu.tfbus_module(sn)` — stats of one module, nil if not declared. */
static int l_tfbus_module(lua_State *L)
{
    const tfbus_module_t *module = tfbus_master_find_sn((uint32_t)luaL_checkinteger(L, 1));
    if (!module) {
        return 0;
    }
    lua_newtable(L);
    lua_pushboolean(L, module->online);
    lua_setfield(L, -2, "online");
    set_int(L, "fails", module->fails_total);
    set_int(L, "polls", module->polls);
    set_int(L, "addr", module->addr);
    set_int(L, "hw_type", module->hw_type);
    set_int(L, "fw_api_ver", module->fw_api_ver); // from DISCOVER (0 = not seen yet)
    tfbus_device_t device;
    if (tfbus_master_device_find(module->sn, &device) && device.have_ver) {
        set_int(L, "hw_ver", device.hw_ver);
        lua_pushfstring(L, "%d.%d.%d", device.ver[0], device.ver[1], device.ver[2]);
        lua_setfield(L, -2, "fw_ver");
    }
    return 1;
}

/** @brief Lua: `__clu.local_channels(sn)` — see the file comment. */
static int l_local_channels(lua_State *L)
{
    localio_channels_t channels;
    if (!clu_localio_channels((uint32_t)luaL_checkinteger(L, 1), &channels)) {
        return 0;
    }
    lua_newtable(L);
    set_class_channels(L, OM_CLASS_DIN, channels.digital_inputs);
    set_class_channels(L, OM_CLASS_DOUT, channels.digital_outputs);
    set_class_channels(L, OM_CLASS_ANALOG_IN, channels.analog_inputs);
    set_class_channels(L, OM_CLASS_ONE_WIRE_HUB, channels.onewire_hubs);
    return 1;
}

/** @brief t[object_class] = channel_mask on the table on top of the stack. */
static void set_class_channels(lua_State *L, int object_class, uint8_t channel_mask)
{
    lua_pushinteger(L, channel_mask);
    lua_rawseti(L, -2, object_class);
}

/** @brief esp_timer callback: reboot. */
static void do_restart(void *arg)
{
    esp_restart();
}

/** @brief Lua: `__clu.sys_restart()` — run the close hooks and reboot after 1 s. */
static int l_sys_restart(lua_State *L)
{
    static esp_timer_handle_t restart_timer;
    if (!restart_timer) {
        const esp_timer_create_args_t timer_args = {.callback = do_restart, .name = "restart"};
        if (esp_timer_create(&timer_args, &restart_timer) != ESP_OK) {
            return 0;
        }
    }
    ESP_LOGW(TAG, "restart requested from Lua/MQTT");
    clu_lua_shutdown_hooks(L); // pending storage writes
    esp_timer_start_once(restart_timer, 1000 * 1000); // fails harmlessly if already pending
    return 0;
}

static const luaL_Reg s_fns[] = {
    {"sys_info", l_sys_info},
    {"tfbus_module", l_tfbus_module},
    {"local_channels", l_local_channels},
    {"sys_restart", l_sys_restart},
    {NULL, NULL},
};

void clu_sysinfo_init(void)
{
    clu_lua_register_ext(s_fns, NULL, 0, "=sysinfo");
}
