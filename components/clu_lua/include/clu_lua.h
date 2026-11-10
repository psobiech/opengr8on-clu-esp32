/**
 * @file clu_lua.h
 * @brief CLU Lua runtime: boots a:\\MAIN.LUA (or EMERGNCY.LUA), serves req: calls, fires events.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lauxlib.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Runtime state reported to OM. */
typedef enum {
    CLU_LUA_STARTING = 0,   /**< Booting MAIN.LUA. */
    CLU_LUA_OK = 1,         /**< Script running. */
    CLU_LUA_EMERGENCY = 4,  /**< Script failed; running EMERGNCY.LUA. */
} clu_lua_state_t;

/**
 * @brief Per-class behaviour hooks.
 *
 * All run in the Lua task with the object table at stack index `obj`.
 */
typedef struct {
    /** Object created (OBJECT:new). Optional. */
    void (*on_new)(lua_State *L, int obj);
    /** Push value and return true if handled, false to fall back to the stored value. */
    bool (*get)(lua_State *L, int obj, int idx);
    /** Value at stack index `val`. Return true if handled (value not stored generically). */
    bool (*set)(lua_State *L, int obj, int idx, int val);
    /** Args at stack indices [first, first+nargs). Return number of results pushed, -1 if unknown. */
    int (*execute)(lua_State *L, int obj, int idx, int first, int nargs);
} clu_class_ops_t;

/** @brief Runtime configuration and host callbacks. */
typedef struct {
    const char *root;    /**< Drive a:\\ directory, e.g. "/fs/a". */
    unsigned    sn;      /**< For built-in checkAlive(). */
    const char *fw_version;  /**< Firmware version string. */
    /** Send an unsolicited clientReport (body after "resp:0.0.0.0:00000000:"). */
    void (*report)(const char *addr, int port, const char *body);
    /** Called from SYSTEM.Init(), after OM.LUA declared all objects and before OnInit fires. */
    void (*on_init)(void);
    /** Lua call on another CLU (ip in host order, 0xC0A80191 = 192.168.1.145): queue the script and
     *  return a call id > 0 (0 = not sent). The reply goes back with clu_lua_post_remote(id, ...). */
    uint32_t (*remote)(uint32_t ip, const char *script);
    /** Local CLU feature set from Lua (CLUxxx:set(idx, v)); str is NULL for non-string values. */
    void (*clu_feature)(int idx, const char *str, double num);
    /** CloudConnection (feature 19): true while the cloud link (MQTT broker) is up. Optional. */
    bool (*cloud_connected)(void);
    /** clu_lua_reload(): old VM closed, new one not created yet. Forget everything OM.LUA declared
     *  (e.g. TFBUS modules). Return false if that failed (the caller then reboots). Optional. */
    bool (*on_reload)(void);
} clu_lua_cfg_t;

/**
 * @brief Register an extension.
 *
 * C functions are merged into the private __clu table; the embedded Lua chunk runs after
 * objects.lua (and so can capture __clu). Call before clu_lua_start(); up to 4 extensions.
 *
 * @param fns  NULL-terminated C functions.
 * @param src  Lua source; must stay valid for the program's lifetime.
 * @param len  Length of @p src.
 * @param name Chunk name (error messages).
 */
void clu_lua_register_ext(const luaL_Reg *fns, const char *src, size_t len, const char *name);

/**
 * @brief Register class hooks. Call before clu_lua_start(); up to 128 classes.
 * @param class_id OM object class id.
 * @param ops      Hooks; must stay valid for the program's lifetime.
 */
void clu_lua_register_class(int class_id, const clu_class_ops_t *ops);

/**
 * @brief Start the Lua task and boot MAIN.LUA.
 * @param cfg Configuration.
 */
void clu_lua_start(const clu_lua_cfg_t *cfg);

/** @brief Current runtime state. */
clu_lua_state_t clu_lua_state(void);

/**
 * @brief Soft reset.
 *
 * Unwind the running script at its next SYSTEM.Loop, close the VM, call on_reload and boot
 * MAIN.LUA again in a fresh VM (emergency state cleared, Uptime restarted). Returns once the old
 * VM is gone. Not from the Lua task.
 *
 * @param timeout_ms Max wait for the script to reach SYSTEM.Loop.
 * @return false if the script did not get to SYSTEM.Loop in time or on_reload failed: reboot then.
 */
bool clu_lua_reload(int timeout_ms);

/**
 * @brief Stop the runtime before a reboot.
 *
 * Unwind the script like clu_lua_reload, run the close hooks (pending storage writes), close the
 * VM and stop. Not from the Lua task.
 *
 * @param timeout_ms Max wait.
 * @return false on timeout (reboot anyway).
 */
bool clu_lua_halt(int timeout_ms);

/**
 * @brief Run the close hooks in the calling Lua task (reboot requested from Lua itself).
 * @param L Lua state.
 */
void clu_lua_shutdown_hooks(lua_State *L);

/**
 * @brief Evaluate a Lua expression in the VM. Thread-safe.
 *
 * @param expr       Lua expression.
 * @param from_ip    Caller address, exposed as __req_from.
 * @param out        Result: Lua tostring() of the first return value ("nil" if none).
 * @param cap        Capacity of @p out.
 * @param timeout_ms Max wait for the Lua task.
 * @return ESP_OK, or an error on timeout / failure.
 */
esp_err_t clu_lua_call(const char *expr, const char *from_ip, char *out, size_t cap, int timeout_ms);

/**
 * @name Thread-safe notifications
 * Delivered on the next SYSTEM.Loop().
 * @{
 */
/** @brief Fire event @p event_idx of object @p obj_name. */
void clu_lua_post_event(const char *obj_name, int event_idx);
/** @brief Set feature @p feature_idx of object @p obj_name to @p value. */
void clu_lua_post_number(const char *obj_name, int feature_idx, double value);
/** @brief Set feature @p feature_idx of object @p obj_name to the string @p value (max 15 chars). */
void clu_lua_post_string(const char *obj_name, int feature_idx, const char *value);
/**
 * @brief Result of a remote CLU call, delivered to the Lua thread waiting for it.
 * @param id     Call id from clu_lua_cfg_t::remote.
 * @param result The "resp:" payload; NULL = timeout / send failure.
 */
void clu_lua_post_remote(uint32_t id, const char *result);
/** @} */

/**
 * @name Hook helpers
 * Read fields of the object table at stack index `obj`.
 * @{
 */
/** @brief Object name. */
const char *clu_obj_name(lua_State *L, int obj);
/** @brief Object class id. */
int clu_obj_class(lua_State *L, int obj);
/** @} */

#ifdef __cplusplus
}
#endif
