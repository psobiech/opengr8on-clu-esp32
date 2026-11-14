/**
 * @file tfbus_master.c
 * @brief TFBUS bus master: module registry, Lua class hooks, discovery and the polling bus task.
 *
 * The CLU owns the module list: modules found by DISCOVER (or declared in OM.LUA but not answering
 * it) are kept with their bus address in NVS, so addresses survive reboots and OM, which builds
 * OM.LUA from the CLU's tfbusDevices, stays in sync. OM.LUA only binds names to SNs. Modules are
 * dropped only by req_refresh_modules (not found on the bus), and even then not forgotten: the
 * entry stays in place marked gone, keeping its address, so a module that comes back gets the
 * same address; a new module takes a gone one's slot only when no address is left. Entries never
 * move, since drivers and deferred calls keep pointers to them. The bus runs from boot whatever
 * Lua does; driver init waits for SYSTEM.Init (the module configuration).
 *
 * Threads: the Lua task binds modules / IO objects (class hooks) and calls drivers' io_* hooks
 * under s_drv_lock; the bus task owns the UART, runs discovery, polls modules and runs driver
 * callbacks and deferred driver work under the same lock. Commands from any task go through the
 * s_cmds queue.
 */
#include "tfbus_master.h"

#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "nvs.h"
#include "tfbus_uart.h"

static const char *TAG = "tfbus"; // tfbus_master_set_log_tag()

#define POLL_TIMEOUT_MS     20
#define ACK_TIMEOUT_MS      20
#define CMD_TRIES           3
#define OFFLINE_AFTER       3      // missed polls
#define OFFLINE_POLL_EVERY  20     // poll offline modules once per N rounds
#define RESET_SETTLE_MS     2000   // RESET -> SEEK gap seen on the real CLU
#define REFRESH_SETTLE_MS   1200   // req_refresh_modules: RESET -> first ADDR_ASSIGN (real CLU)
#define MAX_DRIVERS         32
#define DISCOVER_TIMEOUT_MS 40
#define DISCOVER_RETRIES    3
#define SUMMARY_EVERY_MS    600000 // "round N: x/y online" heartbeat (also logged on change)
#define NVS_NS              "tfbus"
#define NVS_KEY             "mods"

/** @brief Queued CLU->module frame. */
typedef struct {
    uint8_t len;
    uint8_t frame[TFBUS_MAX_FRAME];
} cmd_t;

/** @brief Deferred driver call (tfbus_master_defer()). */
typedef struct {
    tfbus_defer_fn_t fn;
    tfbus_module_t *m;
    void *arg;
    uint32_t tag;
} defer_t;

static tfbus_module_t s_mod[TFBUS_MAX_MODULES];
static int s_nmod;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_cmds;
static QueueHandle_t s_defer;
// Serialises driver state between the Lua task (io_new/io_set/io_execute) and deferred calls
// (driver timers) in the bus task.
static SemaphoreHandle_t s_drv_lock;
// A deferred call was dropped (queue full): run_deferred() asks the drivers for overdue timers.
static volatile bool s_defer_lost;
// Serialises reg_save() (Lua task: new module; bus task: discovery) over snapshot + NVS write.
static SemaphoreHandle_t s_save_lock;
static TaskHandle_t s_task;
static volatile bool s_restart;
static volatile bool s_discover = true; // run discovery on the next bus-task cycle (boot, refresh)
static volatile bool s_refresh; // next discovery was requested by req_refresh_modules
// Bus task touching no module state (tfbus_master_reset() clearing driver state).
static volatile bool s_park;
static volatile bool s_parked;
// SYSTEM.Init ran in this Lua VM: module configuration known, drivers may init the modules.
static volatile bool s_started;
static volatile bool s_reinit; // run driver init for all modules before the next poll round
static const tfbus_driver_t *s_drivers[MAX_DRIVERS];
static int s_ndrivers;

// Modules found on the bus (CONFIG.JSON "tfbusDevices"), in address order. Rebuilt by every
// discovery; read under s_lock.
static tfbus_device_t s_dev[TFBUS_MAX_MODULES];
static int s_ndev;
static SemaphoreHandle_t s_scan_done;
static void (*s_scan_cb)(void);
static void (*s_refresh_cb)(void);

void tfbus_driver_register(const tfbus_driver_t *drv)
{
    if (s_ndrivers < MAX_DRIVERS) {
        s_drivers[s_ndrivers++] = drv;
    }
}

/** @brief Registered driver for @p hw_type, NULL if none. */
static const tfbus_driver_t *driver_for(uint8_t hw_type)
{
    for (int i = 0; i < s_ndrivers; i++) {
        if (s_drivers[i]->hw_type == hw_type) {
            return s_drivers[i];
        }
    }
    return NULL;
}

int tfbus_master_count(void)
{
    return s_nmod;
}

/** @brief Slot holds a module on the bus side (not empty, not gone after a refresh). */
static bool live(const tfbus_module_t *module)
{
    return module->sn && !module->gone;
}

tfbus_module_t *tfbus_master_get(int module_index)
{
    return (module_index >= 0 && module_index < s_nmod && live(&s_mod[module_index])) ? &s_mod[module_index] : NULL;
}

tfbus_module_t *tfbus_master_find_sn(uint32_t serial_number)
{
    for (int i = 0; i < s_nmod && serial_number; i++) {
        if (live(&s_mod[i]) && s_mod[i].sn == serial_number) {
            return &s_mod[i];
        }
    }
    return NULL;
}

tfbus_module_t *tfbus_master_find_name(const char *name)
{
    for (int i = 0; i < s_nmod && name[0]; i++) {
        if (live(&s_mod[i]) && strcmp(s_mod[i].name, name) == 0) {
            return &s_mod[i];
        }
    }
    return NULL;
}

/** @brief Module as stored in NVS (blob NVS_NS/NVS_KEY, one per registry entry). */
typedef struct __attribute__((packed)) {
    uint32_t sn;
    uint8_t addr; // | SAVED_GONE
    uint8_t hw_type, fw_type, fw_api_ver;
} saved_mod_t;
#define SAVED_GONE 0x80

/**
 * @brief Write the registry (gone entries too) to NVS (any task, not under s_lock).
 *
 * Whole save under s_save_lock: the static buffer is shared, and the snapshot taken last must
 * also be the one written last (else an older registry could win in NVS).
 */
static void reg_save(void)
{
    static saved_mod_t buf[TFBUS_MAX_MODULES];
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int saved_entry_count = 0;
    for (int i = 0; i < s_nmod; i++) {
        const tfbus_module_t *module = &s_mod[i];
        if (module->sn) {
            buf[saved_entry_count++] = (saved_mod_t){.sn = module->sn, .addr = (uint8_t)(module->addr | (module->gone ? SAVED_GONE : 0)),
                                     .hw_type = module->hw_type, .fw_type = module->fw_type, .fw_api_ver = module->fw_api_ver};
        }
    }
    xSemaphoreGive(s_lock);
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGE(TAG, "registry not saved (NVS)");
        xSemaphoreGive(s_save_lock);
        return;
    }
    if (nvs_set_blob(nvs_handle, NVS_KEY, buf, (size_t)saved_entry_count * sizeof *buf) != ESP_OK || nvs_commit(nvs_handle) != ESP_OK) {
        ESP_LOGE(TAG, "registry not saved (NVS write)");
    }
    nvs_close(nvs_handle);
    xSemaphoreGive(s_save_lock);
}

/** @brief Load the registry saved by reg_save() (tfbus_master_init()). */
static void reg_load(void)
{
    static saved_mod_t buf[TFBUS_MAX_MODULES];
    size_t len = sizeof buf;
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(nvs_handle, NVS_KEY, buf, &len) != ESP_OK) {
        len = 0;
    }
    nvs_close(nvs_handle);
    for (size_t i = 0; i < len / sizeof *buf; i++) {
        const saved_mod_t *saved_entry = &buf[i];
        uint8_t addr = saved_entry->addr & ~SAVED_GONE;
        bool dup = !saved_entry->sn || !addr || addr > TFBUS_MAX_MODULES;
        for (int j = 0; j < s_nmod && !dup; j++) {
            dup = s_mod[j].sn == saved_entry->sn || s_mod[j].addr == addr;
        }
        if (dup) {
            continue; // corrupt entry: the module is found again by DISCOVER
        }
        tfbus_module_t *module = &s_mod[s_nmod++];
        memset(module, 0, sizeof *module);
        module->sn = saved_entry->sn;
        module->addr = addr;
        module->gone = (saved_entry->addr & SAVED_GONE) != 0;
        module->hw_type = saved_entry->hw_type;
        module->fw_type = saved_entry->fw_type;
        module->fw_api_ver = saved_entry->fw_api_ver;
        module->drv = driver_for(module->hw_type);
    }
    ESP_LOGI(TAG, "%d module(s) in the saved registry", s_nmod);
}

/**
 * @brief Add a module to the registry, or bring a gone one back. Under s_lock.
 *
 * A gone entry with this SN (dropped by a refresh) comes back with its old address. A new module
 * gets the lowest address no entry holds, in an empty slot; with none left it takes over the
 * slot and address of a gone entry.
 *
 * @return The entry; NULL if all addresses are held by modules on the bus.
 */
static tfbus_module_t *mod_add(uint32_t serial_number, uint8_t hw_type)
{
    uint64_t used = 0;
    int slot = s_nmod < TFBUS_MAX_MODULES ? s_nmod : -1, gone = -1;
    for (int i = 0; i < s_nmod; i++) {
        tfbus_module_t *module = &s_mod[i];
        if (module->sn == serial_number && module->gone) {
            module->gone = false;
            module->hw_type = hw_type;
            module->drv = driver_for(hw_type);
            module->fails = 0;
            return module;
        }
        if (module->sn) {
            used |= 1ULL << module->addr;
        } else if (slot == s_nmod || slot < 0) {
            slot = i;
        }
        if (module->gone && gone < 0) {
            gone = i;
        }
    }
    int addr = 1;
    while (addr <= TFBUS_MAX_MODULES && (used & (1ULL << addr))) {
        addr++;
    }
    if (slot < 0 || addr > TFBUS_MAX_MODULES) {
        if (gone < 0) {
            return NULL;
        }
        slot = gone; // registry full: forget the first gone module for good
        addr = s_mod[gone].addr;
        ESP_LOGI(TAG, "sn=%lu forgotten, addr %u reused", (unsigned long)s_mod[gone].sn, addr);
    }
    tfbus_module_t *module = &s_mod[slot];
    memset(module, 0, sizeof *module);
    module->hw_type = hw_type;
    module->addr = (uint8_t)addr;
    module->drv = driver_for(hw_type);
    module->sn = serial_number; // last: the bus task skips slots with sn 0 and reads the rest without the lock
    if (slot == s_nmod) {
        s_nmod++;
    }
    return module;
}

/**
 * @brief req_refresh_modules: drop a module not found on the bus (bus task).
 *
 * Driver deinit and context freed, Lua name cleared; the entry stays in place marked gone (SN,
 * address kept, saved) so the module gets the same address if it comes back. Its Lua objects
 * resolve to nothing from now on (io_module() by SN).
 */
static void mod_forget(tfbus_module_t *module)
{
    ESP_LOGW(TAG, "refresh: sn=%lu addr=%u not found, dropped", (unsigned long)module->sn, module->addr);
    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    if (module->drv_ctx && module->drv && module->drv->deinit) {
        module->drv->deinit(module);
    }
    xSemaphoreGive(s_drv_lock);
    vTaskDelay(pdMS_TO_TICKS(20)); // a driver timer callback that fired before deinit may still run
    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    void *ctx = module->drv_ctx;
    module->drv_ctx = NULL;
    module->name[0] = 0;
    module->online = false;
    module->assigned = false;
    module->status_len = 0;
    module->gone = true;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_drv_lock);
    free(ctx);
}

static void drain_cmds(void);

esp_err_t tfbus_master_queue(const uint8_t *frame, size_t len)
{
    if (!s_cmds || len > TFBUS_MAX_FRAME) {
        return ESP_ERR_INVALID_ARG;
    }
    cmd_t command = {.len = (uint8_t)len};
    memcpy(command.frame, frame, len);
    if (xTaskGetCurrentTaskHandle() == s_task) {
        // Bus task (driver init / on_status): never block on its own queue. A full queue is
        // flushed to the bus right here (SMART_PANEL init alone is ~275 frames per panel).
        if (xQueueSend(s_cmds, &command, 0) == pdTRUE) {
            return ESP_OK;
        }
        drain_cmds();
        return xQueueSend(s_cmds, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    return xQueueSend(s_cmds, &command, pdMS_TO_TICKS(50)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool tfbus_master_defer(tfbus_defer_fn_t deferred_callback, tfbus_module_t *module, void *arg, uint32_t tag)
{
    defer_t deferred_call = {.fn = deferred_callback, .m = module, .arg = arg, .tag = tag};
    if (!s_defer || xQueueSend(s_defer, &deferred_call, 0) != pdTRUE) {
        s_defer_lost = true; // recovered by deadline: run_deferred() -> tfbus_driver_t::overdue
        ESP_LOGW(TAG, "defer queue full, call dropped (timers recovered by deadline)");
        return false;
    }
    return true;
}

/** @brief Run queued deferred calls under s_drv_lock (bus task, between polls). */
static void run_deferred(void)
{
    // Try-lock: a Lua hook holding the lock may itself wait (up to 50 ms) for room in s_cmds,
    // which only this task drains. Busy -> leave the calls queued for the next poll.
    if (xSemaphoreTake(s_drv_lock, 0) != pdTRUE) {
        return;
    }
    defer_t deferred_call;
    while (xQueueReceive(s_defer, &deferred_call, 0) == pdTRUE) {
        if (!live(deferred_call.m)) {
            continue; // module dropped by a refresh
        }
        deferred_call.fn(deferred_call.m, deferred_call.arg, deferred_call.tag);
    }
    if (s_defer_lost) {
        s_defer_lost = false; // before the scan: a drop during it triggers another one
        for (int i = 0; i < s_nmod; i++) {
            tfbus_module_t *module = &s_mod[i];
            if (live(module) && module->drv_ctx && module->drv && module->drv->overdue) {
                module->drv->overdue(module);
            }
        }
    }
    xSemaphoreGive(s_drv_lock);
}

// ---------------------------------------------------------------------------
// Lua class 2: OBJECT:new(2, sn, hwType, "snhex")
// ---------------------------------------------------------------------------
/**
 * @brief Lua class 2 hook: OBJECT:new(2, sn, hwType, "snhex") binds a Lua name to a bus module.
 *
 * The module normally is in the CLU's registry already (OM.LUA is built from tfbusDevices): only
 * its name is bound and its address stored as obj._addr. A module the CLU does not know (never
 * answered DISCOVER, registry lost) is added at the lowest free address and saved. Non-TFBUS types
 * (> 0xFF, e.g. Z-Wave) are skipped.
 */
static void module_on_new(lua_State *L, int obj)
{
    lua_getfield(L, obj, "_sn");
    lua_Integer serial_number = lua_tointeger(L, -1);
    lua_getfield(L, obj, "_hwType");
    lua_Integer hardware_type = lua_tointeger(L, -1);
    lua_pop(L, 2);

    if (hardware_type <= 0 || hardware_type > 0xFF) {
        // e.g. 0x008500030011 = Z-Wave node, not on the RS485 bus (skipped by the real CLU too)
        ESP_LOGI(TAG, "skip non-TFBUS module %s (type 0x%llx)", clu_obj_name(L, obj), (long long)hardware_type);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tfbus_module_t *module = tfbus_master_find_sn((uint32_t)serial_number);
    bool added = !module;
    if (module && module->hw_type != (uint8_t)hardware_type) {
        ESP_LOGW(TAG, "module sn=%lu declared with hw 0x%02x, bus says 0x%02x: keeping the bus type",
                 (unsigned long)module->sn, (unsigned)hardware_type, module->hw_type);
    }
    if (!module) {
        module = mod_add((uint32_t)serial_number, (uint8_t)hardware_type);
    }
    if (!module) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "too many modules");
        return;
    }
    strlcpy(module->name, clu_obj_name(L, obj), sizeof module->name);
    uint8_t addr = module->addr;
    xSemaphoreGive(s_lock);
    if (added) {
        reg_save();
    }

    lua_pushinteger(L, addr);
    lua_setfield(L, obj, "_addr");
    ESP_LOGI(TAG, "module %s sn=%lu hw=0x%02x addr=%u%s driver=%s", module->name, (unsigned long)module->sn,
             module->hw_type, addr, added ? " (new, not found on the bus)" : "", module->drv ? module->drv->name : "-");
}

static const clu_class_ops_t s_module_ops = {.on_new = module_on_new};

// ---------------------------------------------------------------------------
// IO objects: OBJECT:new(class, moduleObj, channel, name) -> module driver
// ---------------------------------------------------------------------------
void tfbus_obj_store(lua_State *L, int obj, int idx, lua_Number numeric_value)
{
    lua_getfield(L, obj, "_f");
    if (numeric_value == (lua_Number)(lua_Integer)numeric_value) {
        lua_pushinteger(L, (lua_Integer)numeric_value);
    } else {
        lua_pushnumber(L, numeric_value);
    }
    lua_rawseti(L, -2, idx);
    lua_pop(L, 1);
}

/**
 * @brief Resolve the hosting module of an IO object; NULL for virtual objects.
 * @param L   Lua state.
 * @param obj Stack index of the object table.
 * @param ch  Out: channel on the module.
 */
static tfbus_module_t *io_module(lua_State *L, int obj, int *channel_index)
{
    lua_getfield(L, obj, "_module");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return NULL;
    }
    lua_getfield(L, -1, "_sn");
    tfbus_module_t *module = tfbus_master_find_sn((uint32_t)lua_tointeger(L, -1));
    lua_pop(L, 2);
    lua_getfield(L, obj, "_channel");
    *channel_index = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return (module && module->drv) ? module : NULL;
}

/** @brief IO class hook on_new: forward to the driver's io_new. */
static void io_on_new(lua_State *L, int obj)
{
    int channel_index;
    tfbus_module_t *module = io_module(L, obj, &channel_index);
    if (module && module->drv->io_new) {
        xSemaphoreTake(s_drv_lock, portMAX_DELAY);
        module->drv->io_new(module, clu_obj_class(L, obj), channel_index, clu_obj_name(L, obj));
        xSemaphoreGive(s_drv_lock);
    }
}

/**
 * @brief Arguments / result of a protected driver io_set / io_execute call.
 *
 * Driver io_set / io_execute run under s_drv_lock. They may raise Lua errors (luaL_check*),
 * which would longjmp past the unlock, so they run in a protected call: [call, obj, args...]
 * -> obj at index 2, args from 3; the error is re-raised after unlocking.
 */
typedef struct {
    tfbus_module_t *m;
    int cls, ch, idx, nargs;
    bool execute;
    int ret; // io_set: handled; io_execute: results pushed (-1 unknown)
} io_call_t;

/**
 * @brief lua_pcall body: invoke the driver hook described by the ::io_call_t at stack index 1.
 */
static int io_call_p(lua_State *L)
{
    io_call_t *call_context = lua_touserdata(L, 1);
    if (call_context->execute) {
        call_context->ret = call_context->m->drv->io_execute(call_context->m, L, 2, call_context->cls, call_context->ch, call_context->idx, 3, call_context->nargs);
        return call_context->ret > 0 ? call_context->ret : 0;
    }
    call_context->ret = call_context->m->drv->io_set(call_context->m, L, 2, call_context->cls, call_context->ch, call_context->idx, 3);
    return 0;
}

/**
 * @brief Run a driver io hook in a protected call under s_drv_lock; re-raise its error after unlocking.
 * @return ::io_call_t::ret.
 */
static int io_call(lua_State *L, io_call_t *call_context, int obj, int first)
{
    lua_pushcfunction(L, io_call_p);
    lua_pushlightuserdata(L, call_context);
    lua_pushvalue(L, obj);
    for (int i = 0; i < call_context->nargs; i++) {
        lua_pushvalue(L, first + i);
    }
    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    int call_status = lua_pcall(L, 2 + call_context->nargs, LUA_MULTRET, 0);
    xSemaphoreGive(s_drv_lock);
    if (call_status != LUA_OK) {
        lua_error(L); // error object on top
    }
    return call_context->ret;
}

/** @brief IO class hook set: forward to the driver's io_set. */
static bool io_set(lua_State *L, int obj, int idx, int val)
{
    int channel_index;
    tfbus_module_t *module = io_module(L, obj, &channel_index);
    if (!module || !module->drv->io_set) {
        return false;
    }
    io_call_t call_context = {.m = module, .cls = clu_obj_class(L, obj), .ch = channel_index, .idx = idx, .nargs = 1};
    int top = lua_gettop(L);
    io_call(L, &call_context, obj, val);
    lua_settop(L, top);
    return call_context.ret != 0;
}

/** @brief IO class hook execute: forward to the driver's io_execute. */
static int io_execute(lua_State *L, int obj, int idx, int first, int nargs)
{
    int channel_index;
    tfbus_module_t *module = io_module(L, obj, &channel_index);
    if (!module || !module->drv->io_execute) {
        return -1;
    }
    io_call_t call_context = {.m = module, .cls = clu_obj_class(L, obj), .ch = channel_index, .idx = idx, .nargs = nargs,
                   .execute = true};
    int top = lua_gettop(L);
    io_call(L, &call_context, obj, first);
    if (call_context.ret <= 0) {
        lua_settop(L, top);
    }
    return call_context.ret; // results (if any) are on top
}

static const clu_class_ops_t s_io_ops = {.on_new = io_on_new, .set = io_set, .execute = io_execute};

// ---------------------------------------------------------------------------
// Bus task
// ---------------------------------------------------------------------------
/** @brief True if the reply is an empty ACK frame (AA 00 01 xor). */
static bool is_ack(const uint8_t *response_frame, int response_length)
{
    return response_length == 4 && response_frame[0] == 0xAA && response_frame[1] == 0x00 && response_frame[2] == 0x01;
}

/**
 * @brief Send one queued frame: broadcasts fire-and-forget, others retried up to CMD_TRIES until answered.
 */
static void send_cmd(const cmd_t *queued_command)
{
    uint8_t rx[TFBUS_MAX_FRAME];
    if (queued_command->frame[1] == TFBUS_BROADCAST && queued_command->frame[2] != TFBUS_T_ADDR_ASSIGN &&
        queued_command->frame[2] != TFBUS_T_SEEK) {
        tfbus_uart_send(queued_command->frame, queued_command->len);
        return;
    }
    for (int t = 0; t < CMD_TRIES; t++) {
        int response_length = tfbus_uart_transact(queued_command->frame, queued_command->len, rx, sizeof rx, ACK_TIMEOUT_MS);
        if (response_length > 0) {
            return; // ACK (or any reply)
        }
    }
    ESP_LOGW(TAG, "no ACK for cmd type 0x%02x to addr %u", queued_command->frame[2], queued_command->frame[1]);
}

/** @brief Send every queued command now (bus task only). */
static void drain_cmds(void)
{
    cmd_t queued_command;
    while (xQueueReceive(s_cmds, &queued_command, 0) == pdTRUE) {
        send_cmd(&queued_command);
    }
}

/** @brief Update the online flag and notify the driver on change. */
/** @brief Driver init for one module (bus task), under s_drv_lock; flushes the queued frames. */
static void drv_init(tfbus_module_t *module)
{
    module->init_pending = false;
    if (!live(module) || !module->drv || !module->drv->init) {
        return;
    }
    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    module->drv->init(module);
    xSemaphoreGive(s_drv_lock);
    drain_cmds(); // init runs in this task: flush before the queue fills up
}

static void set_online(tfbus_module_t *module, bool on)
{
    if (module->online == on) {
        return;
    }
    module->online = on;
    ESP_LOGW(TAG, "addr %u sn=%lu %s", module->addr, (unsigned long)module->sn, on ? "online" : "OFFLINE");
    if (module->drv && module->drv->on_online) {
        xSemaphoreTake(s_drv_lock, portMAX_DELAY);
        module->drv->on_online(module, on);
        xSemaphoreGive(s_drv_lock);
    }
}

/** @brief ADDR_ASSIGN the module's SN to its address; true if ACKed (up to CMD_TRIES). */
static bool assign(tfbus_module_t *module)
{
    uint8_t assign_frame[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];
    size_t frame_length = tfbus_addr_assign(assign_frame, sizeof assign_frame, module->sn, module->addr);
    module->assigned = false;
    for (int t = 0; t < CMD_TRIES && !module->assigned; t++) {
        int response_length = tfbus_uart_transact(assign_frame, frame_length, rx, sizeof rx, ACK_TIMEOUT_MS);
        module->assigned = is_ack(rx, response_length);
    }
    return module->assigned;
}

static void poll_module(tfbus_module_t *module);

/** @brief Find or append the s_dev entry for @p sn; NULL if the table is full. */
static tfbus_device_t *dev_get(uint32_t serial_number)
{
    for (int i = 0; i < s_ndev; i++) {
        if (s_dev[i].sn == serial_number) {
            return &s_dev[i];
        }
    }
    if (s_ndev >= TFBUS_MAX_MODULES) {
        return NULL;
    }
    tfbus_device_t *device = &s_dev[s_ndev++];
    memset(device, 0, sizeof *device);
    device->sn = serial_number;
    device->hw_ver = 1; // real CLU default when VERSION_QUERY has no 4th byte / is not sent
    return device;
}

/**
 * @brief DISCOVER loop (broadcast type 0x08).
 *
 * Every still-unaddressed module answers SN hwType fwType fwApiVer; the CLU addresses it (its
 * registry address, a new module gets the lowest free one) so the next DISCOVER reaches the
 * remaining ones. Ends on silence or an empty ACK ("SEEK - no data - finish").
 *
 * @return true if the registry changed (to be saved).
 */
static bool scan(void)
{
    uint8_t discovery_frame[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];
    size_t frame_length = tfbus_discover(discovery_frame, sizeof discovery_frame);
    int garbled = 0;
    bool added = false;
    for (int found = 0; found < TFBUS_MAX_MODULES;) {
        int response_length = tfbus_uart_transact(discovery_frame, frame_length, rx, sizeof rx, DISCOVER_TIMEOUT_MS);
        const uint8_t *response_payload;
        size_t plen;
        tfbus_ident_t id;
        if (response_length <= 0) {
            break;
        }
        if (!tfbus_parse_response(rx, (size_t)response_length, &response_payload, &plen)) {
            if (++garbled > DISCOVER_RETRIES) { // several modules answering at once
                ESP_LOGW(TAG, "DISCOVER: garbled replies, giving up");
                break;
            }
            continue;
        }
        if (plen == 0 || !tfbus_parse_discover(response_payload, plen, &id)) {
            break; // plain ACK: no (more) unaddressed modules
        }
        found++;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        tfbus_module_t *module = tfbus_master_find_sn(id.sn);
        bool is_new = !module; // or gone: back with its old address
        if (!module) {
            module = mod_add(id.sn, id.hw_type);
        }
        if (module && (module->fw_type != id.fw_type || module->fw_api_ver != id.fw_api_ver)) {
            module->fw_type = id.fw_type;
            module->fw_api_ver = id.fw_api_ver;
            added = true; // identity changed (firmware update): save it
        }
        added |= is_new && module;
        tfbus_device_t *device = dev_get(id.sn);
        if (device) {
            device->hw_type = id.hw_type;
            device->fw_type = id.fw_type;
            device->fw_api_ver = id.fw_api_ver;
            device->known = module != NULL;
        }
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "DISCOVER: sn=%08lx hw=0x%02x fwType=%u fwApiVer=%u%s", (unsigned long)id.sn,
                 id.hw_type, id.fw_type, id.fw_api_ver, is_new ? (module ? " (new)" : "") : "");
        if (!module) {
            ESP_LOGW(TAG, "DISCOVER: no free address");
            break;
        }
        if (!assign(module)) {
            break; // it would answer the next DISCOVER again
        }
    }
    return added;
}

/**
 * @brief Identity of an addressed registry module that DISCOVER did not report.
 *
 * Taken from the previous scan's entry (prev, may be NULL), else from the registry (saved DISCOVER
 * identity, or the type OM.LUA declared). Under s_lock.
 */
static void dev_fill(tfbus_device_t *device, const tfbus_module_t *module, const tfbus_device_t *prev, int nprev)
{
    const tfbus_device_t *previous_device = NULL;
    for (int j = 0; j < nprev && !previous_device; j++) {
        previous_device = prev[j].sn == module->sn && prev[j].hw_type ? &prev[j] : NULL;
    }
    if (previous_device) {
        device->hw_type = previous_device->hw_type;
        device->hw_ver = previous_device->hw_ver;
        device->fw_type = previous_device->fw_type;
        device->fw_api_ver = previous_device->fw_api_ver;
        memcpy(device->ver, previous_device->ver, sizeof device->ver);
        device->have_ver = previous_device->have_ver;
    } else {
        device->hw_type = module->hw_type;
        device->fw_type = module->fw_type ? module->fw_type : module->hw_type == 0x03 ? 3 : 2; // SMART_PANEL 3, IO 2
        device->fw_api_ver = module->fw_api_ver;
    }
}

/**
 * @brief VERSION_QUERY: [fwApiVer verA verB (hwVer)] -> fwVer "fwApiVer.verA.verB".
 *
 * Matches the real CONFIG.JSON. The real CLU skips SMART_PANELs with fwApiVer < 5 (they do not answer).
 */
static void version_query(tfbus_device_t *device, uint8_t addr)
{
    if (device->hw_type == 0x03 && device->fw_api_ver && device->fw_api_ver < 5) {
        return;
    }
    uint8_t version_query_frame[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];
    size_t frame_length = tfbus_version_query(version_query_frame, sizeof version_query_frame, addr);
    int response_length = tfbus_uart_transact(version_query_frame, frame_length, rx, sizeof rx, ACK_TIMEOUT_MS);
    const uint8_t *response_payload;
    size_t plen;
    if (response_length > 0 && tfbus_parse_response(rx, (size_t)response_length, &response_payload, &plen) && plen >= 3) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        device->fw_api_ver = response_payload[0];
        memcpy(device->ver, response_payload, 3);
        device->hw_ver = plen >= 4 ? response_payload[3] : 1;
        device->have_ver = true;
        xSemaphoreGive(s_lock);
    }
}

/** @brief qsort: tfbusDevices in address order (the CLU's module order), unaddressed last. */
static int dev_cmp(const void *left_device, const void *right_device)
{
    unsigned left_address = ((const tfbus_device_t *)left_device)->addr, right_address = ((const tfbus_device_t *)right_device)->addr;
    left_address = left_address ? left_address : 256;
    right_address = right_address ? right_address : 256;
    return (left_address > right_address) - (left_address < right_address);
}

/**
 * @brief Full bus discovery; rebuilds s_dev, adds new modules to the registry (saved).
 *
 * Boot: RESET, SEEK, DISCOVER loop, then ADDR_ASSIGN by SN for the registry modules that did not
 * answer DISCOVER.
 *
 * Refresh (req_refresh_modules, OM "Clear CLU"): what the real CLU sends (capture
 * test/host/fixtures/capture_refresh_modules_clu2.txt): RESET, ~1.2 s, no SEEK, ADDR_ASSIGN by
 * SN for the registry modules first, then DISCOVER for whatever is still unaddressed (new modules;
 * known ones that missed their ADDR_ASSIGN), then the rest again, then VERSION_QUERY. Assigning
 * first also means only leftovers answer DISCOVER, instead of every module colliding after RESET.
 *
 * @param refresh true for the req_refresh_modules sequence.
 */
static void discover(bool refresh)
{
    uint8_t request_frame[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];
    size_t frame_length;

    if (refresh && s_refresh_cb) {
        s_refresh_cb(); // e.g. emulated modules of devices that are gone removed before the RESET
    }
    ESP_LOGI(TAG, "%s: %d module(s) in the registry", refresh ? "refresh" : "discovery", s_nmod);
    frame_length = tfbus_reset(request_frame, sizeof request_frame, TFBUS_BROADCAST);
    tfbus_uart_send(request_frame, frame_length);
    vTaskDelay(pdMS_TO_TICKS(refresh ? REFRESH_SETTLE_MS : RESET_SETTLE_MS));

    if (!refresh) {
        frame_length = tfbus_seek(request_frame, sizeof request_frame);
        int response_length = tfbus_uart_transact(request_frame, frame_length, rx, sizeof rx, ACK_TIMEOUT_MS * 5);
        ESP_LOGI(TAG, "SEEK %s", response_length > 0 ? "acked" : "no reply");
        (void)response_length; // log-only
    }

    for (int i = 0; i < s_nmod; i++) {
        tfbus_module_t *module = &s_mod[i];
        if (!live(module)) {
            continue;
        }
        module->online = false;
        module->fails = 0;
        module->status_len = 0;
        module->assigned = false;
    }
    // Identity from the previous scan. On refresh the registry modules are addressed before
    // DISCOVER, so they never report hwType/fwType/fwApiVer, and old SMART_PANELs (fwApiVer < 5)
    // do not answer VERSION_QUERY either: without this, config.txt would say fwApiVer 0 and OM
    // finds no device interface ("fv00"). The real CLU keeps reporting 03:03:04 after a refresh.
    static tfbus_device_t prev[TFBUS_MAX_MODULES];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int nprev = s_ndev;
    memcpy(prev, s_dev, (size_t)nprev * sizeof *prev);
    s_ndev = 0;
    xSemaphoreGive(s_lock);
    if (refresh) {
        for (int i = 0; i < s_nmod; i++) {
            if (live(&s_mod[i])) {
                assign(&s_mod[i]); // failures retried after DISCOVER
            }
        }
    }
    bool changed = scan();
    // Registry modules that did not answer DISCOVER (e.g. real Grenton modules): by SN, as before.
    for (int i = 0; i < s_nmod; i++) {
        tfbus_module_t *module = &s_mod[i];
        if (live(module) && !module->assigned && !assign(module)) {
            ESP_LOGW(TAG, "ADDR_ASSIGN sn=%lu addr=%u: no ACK", (unsigned long)module->sn, module->addr);
        }
    }
    // req_refresh_modules is the only way out of the registry: what is not on the bus now is
    // dropped (a module that is merely offline for a while stays until then; OM blacklists it
    // meanwhile), keeping its address for when it comes back.
    for (int i = 0; refresh && i < s_nmod; i++) {
        if (live(&s_mod[i]) && !s_mod[i].assigned) {
            mod_forget(&s_mod[i]);
            changed = true;
        }
    }
    for (int i = 0; i < s_nmod; i++) {
        tfbus_module_t *module = &s_mod[i];
        if (!live(module) || !module->assigned) {
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        tfbus_device_t *device = dev_get(module->sn);
        if (device && !device->hw_type) { // not reported by DISCOVER: last scan's identity, else the registry
            dev_fill(device, module, prev, nprev);
        }
        if (device) {
            device->known = true;
            device->addr = module->addr;
        }
        xSemaphoreGive(s_lock);
    }
    for (int i = 0; i < s_ndev; i++) {
        if (s_dev[i].addr) {
            version_query(&s_dev[i], s_dev[i].addr);
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    qsort(s_dev, (size_t)s_ndev, sizeof *s_dev, dev_cmp);
    xSemaphoreGive(s_lock);
    if (changed) {
        reg_save();
    }
    ESP_LOGI(TAG, "discovery: %d module(s) on the bus", s_ndev);
}

/**
 * @brief Try to bring an offline module back.
 *
 * Offline module (absent at discovery, power-cycled, or lost its address after a RESET): it
 * answers nothing until it is addressed again. Re-send ADDR_ASSIGN (only the module with that
 * SN reacts, an addressed one just re-takes the same address) and re-run the driver init, since
 * a module that rebooted has lost its configuration.
 */
static void rejoin(tfbus_module_t *module)
{
    if (!assign(module)) {
        return;
    }
    poll_module(module);
    if (!module->online) {
        return;
    }
    ESP_LOGI(TAG, "addr %u sn=%lu rejoined", module->addr, (unsigned long)module->sn);
    if (s_started) {
        drv_init(module);
    }
    // Absent at the last discovery (plugged in later): list it in tfbusDevices now, so
    // CONFIG.JSON / config.txt do not wait for the next req_refresh_modules.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tfbus_device_t *device = dev_get(module->sn);
    bool fresh = device && !device->addr;
    if (fresh) {
        if (!device->hw_type) {
            dev_fill(device, module, NULL, 0);
        }
        device->known = true;
        device->addr = module->addr;
    }
    xSemaphoreGive(s_lock);
    if (fresh) {
        version_query(device, module->addr);
        if (s_scan_cb) {
            s_scan_cb();
        }
    }
}



/**
 * @brief STATUS_POLL one module: track online/fails, keep the last status and pass every reply to the driver.
 */
static void poll_module(tfbus_module_t *module)
{
    uint8_t status_poll_frame[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];
    size_t frame_length = tfbus_status_poll(status_poll_frame, sizeof status_poll_frame, module->addr);
    int response_length = tfbus_uart_transact(status_poll_frame, frame_length, rx, sizeof rx, POLL_TIMEOUT_MS);
    const uint8_t *response_payload;
    size_t plen;
    if (response_length <= 0 || !tfbus_parse_response(rx, (size_t)response_length, &response_payload, &plen)) {
        module->fails_total++;
        if (module->fails < 255) {
            module->fails++;
        }
        if (module->fails >= OFFLINE_AFTER) {
            set_online(module, false);
        }
        return;
    }
    module->fails = 0;
    module->polls++;
    set_online(module, true);

    if (plen > sizeof module->status) {
        plen = sizeof module->status;
    }
    bool changed = plen != module->status_len || memcmp(response_payload, module->status, plen) != 0;
    if (changed) {
        memcpy(module->status, response_payload, plen);
        module->status_len = (uint8_t)plen;
        if (!module->drv) {
            char hex[3 * TFBUS_MAX_PAYLOAD + 1];
            int log_length = 0;
            for (size_t i = 0; i < plen; i++) {
                log_length += snprintf(hex + log_length, sizeof hex - log_length, "%02x ", response_payload[i]);
            }
            ESP_LOGD(TAG, "addr %u status: %s", module->addr, hex);
        }
    }
    // Button events etc. arrive as repeated queued records: drivers see every reply.
    if (module->drv && module->drv->on_status) {
        xSemaphoreTake(s_drv_lock, portMAX_DELAY);
        module->drv->on_status(module, response_payload, plen);
        xSemaphoreGive(s_drv_lock);
    }
}

/**
 * @brief Bus task: discover, then poll the registry round-robin until restarted.
 *
 * Runs from tfbus_master_init(), whatever Lua does. Driver init runs once SYSTEM.Init gave the
 * module configuration (tfbus_master_start()), and again after every discovery (RESET) from then
 * on. Queued commands and deferred driver calls are serviced between polls; offline modules get a
 * rejoin attempt every OFFLINE_POLL_EVERY rounds. Parked only while tfbus_master_reset() frees
 * the driver state.
 */
static void bus_task(void *arg)
{
    for (;;) {
        if (s_park) {
            s_parked = true;
            while (s_park) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
            s_parked = false;
        }
        s_restart = false;
        if (s_discover) {
            s_discover = false;
            bool refresh = s_refresh;
            s_refresh = false;
            discover(refresh);
            if (s_scan_cb) {
                s_scan_cb();
            }
            xSemaphoreGive(s_scan_done);
            // First round before init, like the real CLU.
            for (int i = 0; i < s_nmod; i++) {
                if (live(&s_mod[i])) {
                    poll_module(&s_mod[i]);
                }
            }
            s_reinit |= s_started; // the RESET cleared the modules' configuration
        }
        if (s_nmod == 0) {
            ESP_LOGI(TAG, "no modules, bus idle");
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        uint32_t round = 0;
        int last_online = -1;
        TickType_t last_summary = 0;
        while (!s_restart) {
            if (s_reinit) {
                s_reinit = false;
                for (int i = 0; i < s_nmod; i++) {
                    // A module not answering (absent, not addressed, emulated module without its
                    // network) would only collect unACKed init frames: init it once it answers.
                    if (s_mod[i].online) {
                        drv_init(&s_mod[i]);
                    } else {
                        s_mod[i].init_pending = true;
                    }
                }
            }
            for (int i = 0; i < s_nmod && !s_restart; i++) {
                run_deferred(); // driver timer work queues frames: send them right after
                drain_cmds();
                tfbus_module_t *module = &s_mod[i];
                if (!live(module)) {
                    continue; // dropped by a refresh
                }
                if (!module->online && module->fails >= OFFLINE_AFTER) {
                    if (round % OFFLINE_POLL_EVERY == 0) {
                        rejoin(module);
                    }
                    continue;
                }
                poll_module(module);
                if (module->online && module->init_pending && s_started) {
                    ESP_LOGI(TAG, "addr %u sn=%lu answers: driver init", module->addr, (unsigned long)module->sn);
                    drv_init(module);
                }
            }
            round++;
            if (round % 500 == 0) { // summary only on change, else every SUMMARY_EVERY_MS
                int online = 0, total = 0; // modules dropped by a refresh do not count
                for (int i = 0; i < s_nmod; i++) {
                    online += s_mod[i].online;
                    total += live(&s_mod[i]);
                }
                TickType_t tick_count = xTaskGetTickCount();
                if (online != last_online || tick_count - last_summary >= pdMS_TO_TICKS(SUMMARY_EVERY_MS)) {
                    ESP_LOGI(TAG, "round %lu: %d/%d online", (unsigned long)round, online, total);
                    last_online = online;
                    last_summary = tick_count;
                }
            }
            vTaskDelay(1); // let lower-priority tasks run between rounds
        }
    }
}

void tfbus_master_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_scan_done = xSemaphoreCreateBinary();
    s_cmds = xQueueCreate(128, sizeof(cmd_t));
    s_defer = xQueueCreate(32, sizeof(defer_t));
    s_drv_lock = xSemaphoreCreateMutex();
    s_save_lock = xSemaphoreCreateMutex();
    clu_lua_register_class(2, &s_module_ops);
    // Any class may be hosted on a module; virtual-object implementations registered later
    // (e.g. Timer) override these. 0/1 = CLUs, 44 = storage.
    for (int c = 3; c < 128; c++) {
        if (c != 44) {
            clu_lua_register_class(c, &s_io_ops);
        }
    }
    reg_load();
    xTaskCreatePinnedToCore(bus_task, "tfbus", 6144, NULL, 10, &s_task, 1);
}

void tfbus_master_set_log_tag(const char *tag)
{
    TAG = tag;
}

void tfbus_master_start(void)
{
    s_started = true;
    s_reinit = true;
    xTaskNotifyGive(s_task); // wakes an idle bus (modules only just declared)
}

bool tfbus_master_reset(uint32_t timeout_ms)
{
    s_started = false;
    s_reinit = false;
    s_park = true;
    s_restart = true;
    xTaskNotifyGive(s_task);
    // Discovery and the first round do not check s_restart: allow for a full one.
    for (uint32_t t = 0; !s_parked; t += 10) {
        if (t >= timeout_ms) {
            // Driver state untouched: unpark so polling, commands and deferred calls go on (a bus
            // task that parks meanwhile is woken). Driver init waits for tfbus_master_start().
            s_park = false;
            xTaskNotifyGive(s_task);
            ESP_LOGE(TAG, "reset: bus task did not stop");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    for (int i = 0; i < s_nmod; i++) {
        tfbus_module_t *module = &s_mod[i];
        if (module->drv_ctx && module->drv && module->drv->deinit) {
            module->drv->deinit(module);
        }
    }
    // A driver esp_timer callback that fired just before deinit may still be running and using
    // its context: let it finish before the context goes away.
    vTaskDelay(pdMS_TO_TICKS(20));
    // Free the driver state and the Lua names; the modules and their addresses stay (removed only
    // by req_refresh_modules).
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_nmod; i++) {
        tfbus_module_t *module = &s_mod[i];
        free(module->drv_ctx);
        module->drv_ctx = NULL;
        module->name[0] = 0;
    }
    xSemaphoreGive(s_lock);
    xQueueReset(s_cmds); // frames queued for the old drivers
    xQueueReset(s_defer); // deferred calls point at the freed driver contexts
    ESP_LOGI(TAG, "reset: driver state cleared");
    s_park = false; // polling goes on; driver init waits for the next tfbus_master_start()
    xTaskNotifyGive(s_task);
    return true;
}

void tfbus_master_on_scan(void (*scan_callback)(void))
{
    s_scan_cb = scan_callback;
}

void tfbus_master_on_refresh(void (*refresh_callback)(void))
{
    s_refresh_cb = refresh_callback;
}

bool tfbus_master_refresh(uint32_t timeout_ms)
{
    if (!s_task) {
        return false;
    }
    xSemaphoreTake(s_scan_done, 0); // drop a stale completion
    s_refresh = true;
    s_discover = true;
    s_restart = true;
    xTaskNotifyGive(s_task);
    return xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

int tfbus_master_devices(tfbus_device_t *out, int max)
{
    if (!s_lock) {
        return 0; // before tfbus_master_init() (boot-time CONFIG.JSON): no bus scan yet
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int device_count = s_ndev < max ? s_ndev : max;
    memcpy(out, s_dev, (size_t)device_count * sizeof *out);
    xSemaphoreGive(s_lock);
    return device_count;
}
