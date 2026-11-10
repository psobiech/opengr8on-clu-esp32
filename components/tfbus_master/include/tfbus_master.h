/**
 * @file tfbus_master.h
 * @brief TFBUS bus master: module registry (from OM.LUA), address assignment, status polling.
 *
 * Startup sequence matches captures of a real CLU:
 *   RESET bcast, ~2 s, SEEK bcast (ACK), [DISCOVER loop for unaddressed modules,]
 *   ADDR_ASSIGN(sn, addr) per module in OM.LUA order (ACK),
 *   then continuous STATUS_POLL round-robin with queued commands sent between polls.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lua.h"
#include "tfbus_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Max modules in the registry. */
#define TFBUS_MAX_MODULES 48

struct tfbus_driver;

/** @brief A module declared in OM.LUA. */
typedef struct tfbus_module {
    uint32_t sn;              /**< Serial number. */
    uint8_t  hw_type;         /**< Module hardware type; selects the driver. */
    uint8_t  addr;            /**< 1..48, assigned in OM.LUA order. */
    char     name[24];        /**< Lua global name, e.g. "mm_330017203". */
    bool     assigned;        /**< ACKed ADDR_ASSIGN. */
    bool     online;          /**< Answering polls. */
    uint8_t  fails;           /**< Consecutive missed polls. */
    uint32_t fails_total;     /**< Missed polls since boot (diagnostic pack "fails"). */
    uint32_t polls;           /**< Polls sent. */
    uint8_t  status[TFBUS_MAX_PAYLOAD];  /**< Last STATUS_POLL reply payload. */
    uint8_t  status_len;      /**< Length of @ref status. */
    const struct tfbus_driver *drv;      /**< Driver for @ref hw_type (NULL = none). */
    void    *drv_ctx;         /**< Driver state; freed by the master on reset. */
} tfbus_module_t;

/**
 * @brief Per-hwType behaviour (module driver).
 *
 * All callbacks run in the bus task unless noted otherwise.
 */
typedef struct tfbus_driver {
    uint8_t hw_type;          /**< Handled hardware type. */
    const char *name;         /**< Driver name (logs). */
    const void *cfg;          /**< Driver-specific parameters (shared implementations). */
    /** After address assignment: queue init commands with tfbus_master_queue(). */
    void (*init)(tfbus_module_t *m);
    /** STATUS_POLL reply payload (after the length byte). Called for every reply. */
    void (*on_status)(tfbus_module_t *m, const uint8_t *payload, size_t len);
    /** Module went offline / came back. */
    void (*on_online)(tfbus_module_t *m, bool online);
    /** Registry reset (Lua reload): stop and delete timers etc. Optional; drv_ctx is freed by the
     *  master afterwards. Runs in the Lua task with the bus task parked. */
    void (*deinit)(tfbus_module_t *m);

    /** IO object hosted on the module created: OBJECT:new(class, module, channel, name).
     *  The io_* hooks run in the Lua task; `obj` is the object table's stack index. */
    void (*io_new)(tfbus_module_t *m, int cls, int ch, const char *name);
    /** Feature set. Return true if handled (value not stored generically by the prelude). */
    bool (*io_set)(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int val);
    /** Method call, args at [first, first+nargs). Return number of Lua results pushed, -1 if the
     *  method is unknown. */
    int (*io_execute)(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx,
                      int first, int nargs);
} tfbus_driver_t;

/**
 * @brief Store a feature value on the object table (obj._f[idx]) from a Lua-task hook.
 * @param L   Lua state.
 * @param obj Stack index of the object table.
 * @param idx Feature index.
 * @param v   Value.
 */
void tfbus_obj_store(lua_State *L, int obj, int idx, lua_Number v);

/**
 * @brief Register a module driver (up to 32). Call before tfbus_master_init().
 * @param drv Driver; must stay valid for the program's lifetime.
 */
void tfbus_driver_register(const tfbus_driver_t *drv);

/** @brief Register the class-2 (module) Lua hook. Call before clu_lua_start(). */
void tfbus_master_init(void);

/** @brief Start (or restart) discovery + polling. Called on SYSTEM.Init(). */
void tfbus_master_start(void);

/**
 * @brief Stop the bus and forget all modules.
 *
 * Driver deinit is called and drv_ctx freed, so a reloaded OM.LUA can declare them again; the bus
 * stays idle until tfbus_master_start(). Not from the bus task.
 *
 * @param timeout_ms How long to wait for the bus task to stop.
 * @return false (registry untouched) if the bus task did not stop in time.
 */
bool tfbus_master_reset(uint32_t timeout_ms);

/** @brief A module found on the bus (CONFIG.JSON "tfbusDevices" entry). */
typedef struct {
    uint32_t sn;
    uint8_t  hw_type, hw_ver, fw_type, fw_api_ver;
    uint8_t  ver[3];          /**< VERSION_QUERY bytes -> fwVer "v0.v1.v2" ("0.0.0" if !have_ver). */
    bool     have_ver;        /**< @ref ver is valid. */
    bool     known;           /**< Declared in OM.LUA. */
    uint8_t  addr;            /**< Bus address (0 = none). */
} tfbus_device_t;

/**
 * @brief Snapshot of the modules found by the last discovery. Thread-safe.
 * @param out Output array.
 * @param max Capacity of @p out.
 * @return Number of entries written.
 */
int tfbus_master_devices(tfbus_device_t *out, int max);

/**
 * @brief Set a callback run in the bus task after every discovery (e.g. to rewrite a:\\CONFIG.JSON).
 * @param cb Callback (NULL = none).
 */
void tfbus_master_on_scan(void (*cb)(void));

/**
 * @brief Re-run discovery (RESET, SEEK, DISCOVER, ADDR_ASSIGN, VERSION_QUERY) and wait for it.
 *
 * Not from the bus task.
 *
 * @param timeout_ms Max wait.
 * @return false on timeout.
 */
bool tfbus_master_refresh(uint32_t timeout_ms);

/**
 * @brief Queue a raw CLU->module frame (built with tfbus_frame.h). Thread-safe.
 *
 * Unicast frames are retried until ACKed (max 3 tries).
 *
 * @param frame Frame.
 * @param len   Frame length (<= TFBUS_MAX_FRAME).
 * @return ESP_OK, or an error if the frame is invalid or the queue is full.
 */
esp_err_t tfbus_master_queue(const uint8_t *frame, size_t len);

/**
 * @brief Deferred call run in the bus task.
 * @param m   Module.
 * @param arg User argument.
 * @param tag User tag (e.g. a timer generation to detect stale calls).
 */
typedef void (*tfbus_defer_fn_t)(tfbus_module_t *m, void *arg, uint32_t tag);

/**
 * @brief Run fn(m, arg, tag) in the bus task, between polls.
 *
 * Meant for esp_timer callbacks: never blocks, so the esp_timer task is not held up and the work
 * does not race the bus task's on_status / init. Calls still queued at tfbus_master_reset() are
 * dropped. `tag` lets the callee ignore a call made stale meanwhile (e.g. a timer generation).
 *
 * @return false if the queue is full.
 */
bool tfbus_master_defer(tfbus_defer_fn_t fn, tfbus_module_t *m, void *arg, uint32_t tag);

/**
 * @name Registry lookup
 * Bus task, or with no concurrent registration (the registry changes only while OM.LUA loads, at
 * boot or on a Lua reload).
 * @{
 */
/** @brief Module by serial number, NULL if not declared. */
tfbus_module_t *tfbus_master_find_sn(uint32_t sn);
/** @brief Module by Lua global name, NULL if not declared. */
tfbus_module_t *tfbus_master_find_name(const char *name);
/** @brief Number of declared modules. */
int tfbus_master_count(void);
/** @brief Module @p i (0 <= i < tfbus_master_count()), NULL if out of range. */
tfbus_module_t *tfbus_master_get(int i);
/** @} */

#ifdef __cplusplus
}
#endif
