/**
 * @file clu_lua.c
 * @brief CLU Lua runtime: VM lifecycle, OM object model glue (__clu), req: calls and driver events.
 *
 * One Lua task owns the VM. It loads the embedded prelude.lua (OM object model) and objects.lua
 * (virtual objects) plus registered extensions, then runs a:\\MAIN.LUA (EMERGNCY.LUA after a fatal
 * error). Other tasks talk to it only through queues (req: calls, events, remote-call results),
 * served at every SYSTEM.Loop() tick.
 */
#include "clu_lua.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lualib.h"

static const char *TAG = "lua";

extern const char prelude_lua_start[] asm("_binary_prelude_lua_start");
extern const char prelude_lua_end[] asm("_binary_prelude_lua_end");
extern const char objects_lua_start[] asm("_binary_objects_lua_start");
extern const char objects_lua_end[] asm("_binary_objects_lua_end");

#define MAX_CLASSES   128
#define LOOP_WAIT_MS  10
#define REQS_PER_LOOP 8
#define LUA_STACK     (48 * 1024) // parser/C recursion is bounded by LUAI_MAXCCALLS=120 (lua/CMakeLists)
#define REQ_BUDGET_MS 2500        // req: calls are aborted after this (UDP caller waits 3 s)
#define REQ_HOOK_INSNS 10000

static clu_lua_cfg_t s_cfg;
static const clu_class_ops_t *s_classes[MAX_CLASSES];
static volatile clu_lua_state_t s_state = CLU_LUA_STARTING;
static bool s_emergency;
static QueueHandle_t s_reqs;
static QueueHandle_t s_events;
static QueueHandle_t s_remote_res; // remote_res_t: replies to remote CLU calls
static int64_t s_boot_us;
static volatile bool s_reload;       // clu_lua_reload() pending: unwind the script at SYSTEM.Loop
static SemaphoreHandle_t s_reloaded; // old VM closed, on_reload done
static volatile bool s_reload_ok;
static volatile bool s_halt;        // clu_lua_halt(): after closing the VM, do not boot a new one

#define MAX_EXTS 4
/** @brief Registered extension (clu_lua_register_ext()). */
typedef struct {
    const luaL_Reg *fns;
    const char *src;
    size_t len;
    const char *name;
} ext_t;
static ext_t s_exts[MAX_EXTS];
static int s_n_exts;

// ---------------------------------------------------------------------------
// Request handoff between tasks. The caller may time out and walk away; whoever
// observes the other side finished frees the request.
// ---------------------------------------------------------------------------
/** @brief Request ownership state: whoever sees the other side finished frees the request. */
enum { REQ_PENDING = 0, REQ_DONE = 1, REQ_ABANDONED = 2 };

/** @brief req: call handed from clu_lua_call() to the Lua task. */
typedef struct {
    char *expr;
    char from[16];
    char *out;
    size_t cap;
    SemaphoreHandle_t done;
    int state;
} req_t;

/** @brief Driver notification: event (is_value = false) or feature value update. */
typedef struct {
    char name[32];
    int idx;
    bool is_value;
    double value;
} event_t;

/** @brief Result of a remote CLU call. */
typedef struct {
    uint32_t id;
    char *result; // heap; NULL = timeout / send failure
} remote_res_t;

/** @brief Free a request and its buffers. */
static void req_free(req_t *r)
{
    vSemaphoreDelete(r->done);
    free(r->expr);
    free(r->out);
    free(r);
}

void clu_lua_register_ext(const luaL_Reg *fns, const char *src, size_t len, const char *name)
{
    if (s_n_exts < MAX_EXTS) {
        s_exts[s_n_exts++] = (ext_t){.fns = fns, .src = src, .len = len, .name = name};
    }
}

void clu_lua_register_class(int class_id, const clu_class_ops_t *ops)
{
    if (class_id >= 0 && class_id < MAX_CLASSES) {
        s_classes[class_id] = ops;
    }
}

clu_lua_state_t clu_lua_state(void)
{
    return s_state;
}

esp_err_t clu_lua_call(const char *expr, const char *from_ip, char *out, size_t cap, int timeout_ms)
{
    if (!s_reqs) {
        return ESP_ERR_INVALID_STATE;
    }
    req_t *r = calloc(1, sizeof *r);
    if (!r) {
        return ESP_ERR_NO_MEM;
    }
    r->expr = strdup(expr);
    r->out = malloc(cap);
    r->cap = cap;
    r->done = xSemaphoreCreateBinary();
    strlcpy(r->from, from_ip ? from_ip : "", sizeof r->from);
    if (!r->expr || !r->out || !r->done) {
        if (r->done) {
            vSemaphoreDelete(r->done);
        }
        free(r->expr);
        free(r->out);
        free(r);
        return ESP_ERR_NO_MEM;
    }
    r->out[0] = 0;
    if (xQueueSend(s_reqs, &r, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        req_free(r);
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(r->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        strlcpy(out, r->out, cap);
        req_free(r);
        return ESP_OK;
    }
    int prev = __atomic_exchange_n(&r->state, REQ_ABANDONED, __ATOMIC_ACQ_REL);
    if (prev == REQ_DONE) {
        // Finished just after our timeout: result is valid.
        xSemaphoreTake(r->done, portMAX_DELAY);
        strlcpy(out, r->out, cap);
        req_free(r);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT; // Lua task frees it
}

void clu_lua_post_event(const char *obj_name, int event_idx)
{
    if (!s_events) {
        return;
    }
    event_t e = {.idx = event_idx};
    strlcpy(e.name, obj_name, sizeof e.name);
    xQueueSend(s_events, &e, 0);
}

void clu_lua_post_number(const char *obj_name, int feature_idx, double value)
{
    if (!s_events) {
        return;
    }
    event_t e = {.idx = feature_idx, .is_value = true, .value = value};
    strlcpy(e.name, obj_name, sizeof e.name);
    xQueueSend(s_events, &e, 0);
}

void clu_lua_post_remote(uint32_t id, const char *result)
{
    if (!s_remote_res) {
        return;
    }
    remote_res_t r = {.id = id, .result = result ? strdup(result) : NULL};
    if (xQueueSend(s_remote_res, &r, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "remote result %lu dropped (queue full)", (unsigned long)id);
        free(r.result);
    }
}

/** @brief Discard queued remote-call results (their Lua threads died with the old VM). */
static void drop_remote_results(void)
{
    remote_res_t r;
    while (xQueueReceive(s_remote_res, &r, 0) == pdTRUE) {
        free(r.result);
    }
}

// ---------------------------------------------------------------------------
// Object helpers
// ---------------------------------------------------------------------------
const char *clu_obj_name(lua_State *L, int obj)
{
    lua_getfield(L, obj, "_name");
    const char *n = lua_tostring(L, -1); // string stays referenced by the object table
    lua_pop(L, 1);
    return n ? n : "";
}

int clu_obj_class(lua_State *L, int obj)
{
    lua_getfield(L, obj, "_class");
    int c = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return c;
}

/** @brief Class hooks of the object at @p obj, NULL if none registered. */
static const clu_class_ops_t *ops_for(lua_State *L, int obj)
{
    int c = clu_obj_class(L, obj);
    return (c >= 0 && c < MAX_CLASSES) ? s_classes[c] : NULL;
}

/** @brief Lua: `__clu.new(obj)` — run the class on_new hook. */
static int l_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    const clu_class_ops_t *ops = ops_for(L, 1);
    if (ops && ops->on_new) {
        ops->on_new(L, 1);
    }
    return 0;
}

/** @brief Lua: `__clu.get(obj, idx)` -> handled, value. */
static int l_get(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    int idx = (int)luaL_checkinteger(L, 2);
    const clu_class_ops_t *ops = ops_for(L, 1);
    int top = lua_gettop(L);
    if (ops && ops->get && ops->get(L, 1, idx)) {
        lua_pushboolean(L, 1);
        lua_insert(L, top + 1);
        return 2;
    }
    lua_settop(L, top);
    lua_pushboolean(L, 0);
    return 1;
}

/** @brief Lua: `__clu.set(obj, idx, value)` -> handled. */
static int l_set(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    int idx = (int)luaL_checkinteger(L, 2);
    lua_settop(L, 3);
    const clu_class_ops_t *ops = ops_for(L, 1);
    bool handled = ops && ops->set && ops->set(L, 1, idx, 3);
    lua_settop(L, 3);
    lua_pushboolean(L, handled);
    return 1;
}

/** @brief Lua: `__clu.execute(obj, idx, ...)` -> results. */
static int l_execute(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    int idx = (int)luaL_checkinteger(L, 2);
    int nargs = lua_gettop(L) - 2;
    const clu_class_ops_t *ops = ops_for(L, 1);
    int top = lua_gettop(L);
    int n = ops && ops->execute ? ops->execute(L, 1, idx, 3, nargs) : -1;
    if (n < 0) {
        lua_settop(L, top);
        ESP_LOGD(TAG, "%s:execute(%d) not implemented (class %d)", clu_obj_name(L, 1), idx,
                 clu_obj_class(L, 1));
        return 0;
    }
    return n;
}

/** @brief Lua: `__clu.log(level, ...)` — ESP_LOG the arguments. */
static int l_log(lua_State *L)
{
    int level = (int)luaL_checkinteger(L, 1);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    int n = lua_gettop(L);
    for (int i = 2; i <= n; i++) {
        if (i > 2) {
            luaL_addchar(&b, '\t');
        }
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    const char *msg = lua_tostring(L, -1);
    switch (level) {
    case 1: ESP_LOGE("lua_script", "%s", msg); break;
    case 2: ESP_LOGW("lua_script", "%s", msg); break;
    case 4: ESP_LOGD("lua_script", "%s", msg); break;
    default: ESP_LOGI("lua_script", "%s", msg); break;
    }
    return 0;
}

// Count hook armed while a req: call runs: a runaway script (while true do end) is aborted with a
// Lua error instead of hanging the runtime. MAIN.LUA's own loop is not limited.
static int64_t s_req_deadline_us;
static bool s_req_active;

/**
 * @brief Count hook armed while a req: call runs.
 *
 * A runaway script (while true do end) is aborted with a Lua error instead of hanging the
 * runtime. MAIN.LUA's own loop is not limited.
 */
static void req_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (!s_req_active) {
        // A coroutine started inside a req: (event handler) inherited this hook and was resumed
        // later, e.g. after a remote CLU reply: no req: limit applies to it any more.
        lua_sethook(L, NULL, 0, 0);
        return;
    }
    if (esp_timer_get_time() > s_req_deadline_us) {
        luaL_error(L, "req: time limit (%d ms) exceeded", REQ_BUDGET_MS);
    }
}

/**
 * @brief Body of a req: call, run under lua_pcall.
 *
 * Everything that can raise (allocation, loading, the call itself and the result's __tostring)
 * stays inside the protected call and under the req: time limit, so a bad request can never
 * unwind through SYSTEM.Loop and take MAIN.LUA down.
 */
static int req_protected(lua_State *L)
{
    req_t *r = lua_touserdata(L, 1);
    size_t el = strlen(r->expr);

    lua_pushstring(L, r->from);
    lua_setglobal(L, "__req_from");

    lua_pushliteral(L, "return ");
    lua_pushstring(L, r->expr);
    lua_concat(L, 2);
    size_t cl;
    const char *code = lua_tolstring(L, -1, &cl);
    int st = luaL_loadbufferx(L, code, cl, "=req", "t");
    lua_remove(L, -2);
    if (st != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadbufferx(L, r->expr, el, "=req", "t") != LUA_OK) { // statement, not expression
            return lua_error(L);
        }
    }
    int base = lua_gettop(L) - 1;
    lua_call(L, 0, LUA_MULTRET);
    if (lua_gettop(L) > base) {
        strlcpy(r->out, luaL_tolstring(L, base + 1, NULL), r->cap);
    } else {
        strlcpy(r->out, "nil", r->cap);
    }
    return 0;
}

/**
 * @brief Execute one req: call with the time-limit hook, store the result and hand the request back.
 */
static void exec_request(lua_State *L, req_t *r)
{
    size_t el = strlen(r->expr);
    while (el && (r->expr[el - 1] == '\r' || r->expr[el - 1] == '\n')) {
        r->expr[--el] = 0;
    }

    // Requests run nested in SYSTEM.Loop (MAIN.LUA): keep the caller's hook state.
    lua_Hook prev_hook = lua_gethook(L);
    int prev_mask = lua_gethookmask(L), prev_count = lua_gethookcount(L);
    s_req_deadline_us = esp_timer_get_time() + (int64_t)REQ_BUDGET_MS * 1000;
    lua_sethook(L, req_hook, LUA_MASKCOUNT, REQ_HOOK_INSNS);
    lua_pushcfunction(L, req_protected);
    lua_pushlightuserdata(L, r);
    s_req_active = true;
    int st = lua_pcall(L, 1, 0, 0);
    s_req_active = false;
    lua_sethook(L, prev_hook, prev_mask, prev_count);
    if (st != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        ESP_LOGW(TAG, "req '%s' failed: %s", r->expr, err ? err : "(non-string error)");
        lua_pop(L, 1);
        strlcpy(r->out, "nil", r->cap);
    }

    int prev = __atomic_exchange_n(&r->state, REQ_DONE, __ATOMIC_ACQ_REL);
    if (prev == REQ_ABANDONED) {
        req_free(r);
    } else {
        xSemaphoreGive(r->done);
    }
}

/** @brief Deliver queued driver events / values to Lua (__fire / __update). */
static void drain_events(lua_State *L)
{
    event_t e;
    while (xQueueReceive(s_events, &e, 0) == pdTRUE) {
        lua_getglobal(L, e.is_value ? "__update" : "__fire");
        lua_pushstring(L, e.name);
        lua_pushinteger(L, e.idx);
        int n = 2;
        if (e.is_value) {
            if (e.value == (double)(lua_Integer)e.value) {
                lua_pushinteger(L, (lua_Integer)e.value);
            } else {
                lua_pushnumber(L, e.value);
            }
            n = 3;
        }
        if (lua_pcall(L, n, 0, 0) != LUA_OK) {
            ESP_LOGW(TAG, "event %s/%d: %s", e.name, e.idx, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    remote_res_t r;
    while (xQueueReceive(s_remote_res, &r, 0) == pdTRUE) {
        lua_getglobal(L, "__remote_done"); // resumes the Lua thread waiting for this call
        lua_pushinteger(L, r.id);
        if (r.result) {
            lua_pushstring(L, r.result);
        } else {
            lua_pushnil(L);
        }
        free(r.result);
        if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
            ESP_LOGW(TAG, "remote result %lu: %s", (unsigned long)r.id, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

/**
 * @brief Reload hook: raise on every instruction.
 *
 * The error escapes any pcall in the script (SYSTEM.Loop's own included) and unwinds MAIN.LUA
 * back to run_script().
 */
static void reload_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    luaL_error(L, "reload");
}

/**
 * @brief Lua: `__clu.loop()` — serve queued req: calls and driver events (one SYSTEM.Loop tick).
 */
static int l_loop(lua_State *L)
{
    if (s_reload) {
        lua_sethook(L, reload_hook, LUA_MASKCOUNT, 1);
        return luaL_error(L, "reload");
    }
    req_t *r;
    if (xQueueReceive(s_reqs, &r, pdMS_TO_TICKS(LOOP_WAIT_MS)) == pdTRUE) {
        exec_request(L, r);
        for (int i = 1; i < REQS_PER_LOOP && xQueueReceive(s_reqs, &r, 0) == pdTRUE; i++) {
            exec_request(L, r);
        }
    }
    drain_events(L);
    return 0;
}

/** @brief Lua: `__clu.init()` (SYSTEM.Init) — set the state and call the host on_init. */
static int l_init(lua_State *L)
{
    s_state = s_emergency ? CLU_LUA_EMERGENCY : CLU_LUA_OK;
    ESP_LOGI(TAG, "SYSTEM.Init (%s)", s_emergency ? "emergency" : "ok");
    if (s_cfg.on_init) {
        s_cfg.on_init();
    }
    return 0;
}

/** @brief Lua: `__clu.wait([ms])` — sleep the Lua task. */
static int l_wait(lua_State *L)
{
    lua_Integer ms = luaL_optinteger(L, 1, 0);
    if (ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    return 0;
}

/** @brief Lua: `__clu.report(addr, port, body)` — send a clientReport through the host. */
static int l_report(lua_State *L)
{
    const char *addr = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    const char *body = luaL_checkstring(L, 3);
    if (s_cfg.report) {
        s_cfg.report(addr, port, body);
    }
    return 0;
}

/** @brief Lua: `__clu.millis()` — monotonic milliseconds since boot. */
static int l_millis(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

/**
 * @brief Lua: `__clu.remote(ip, script)` — fire-and-forget Lua call on another CLU.
 *
 * ip is the OM.LUA number (0xC0A80191 = 192.168.1.145) or a dotted string.
 */
static int l_remote(lua_State *L)
{
    uint32_t ip = 0;
    if (lua_type(L, 1) == LUA_TNUMBER) {
        ip = (uint32_t)lua_tointeger(L, 1);
    } else {
        unsigned a, b, c, d;
        if (sscanf(luaL_checkstring(L, 1), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 ||
            c > 255 || d > 255) {
            return luaL_error(L, "bad remote CLU address");
        }
        ip = a << 24 | b << 16 | c << 8 | d;
    }
    const char *script = luaL_checkstring(L, 2);
    uint32_t id = 0;
    if (s_cfg.remote) {
        id = s_cfg.remote(ip, script);
    } else {
        ESP_LOGW(TAG, "remote call dropped (no transport): %s", script);
    }
    if (!id) {
        return 0; // nil: not sent
    }
    lua_pushinteger(L, id);
    return 1;
}

/** @brief Load and run an embedded Lua chunk; false (error logged) on failure. */
static bool load_embedded(lua_State *L, const char *start, const char *end, const char *name)
{
    if (luaL_loadbuffer(L, start, end - start - 1, name) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "%s: %s", name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Class 0: local CLU (clu_ZWAVE_2_ft00000003_fv201 interface)
// ---------------------------------------------------------------------------
/**
 * @brief Class 0 get: computed features (Uptime, State, date/time, FwVersion, UseCloud, CloudConnection).
 */
static bool clu0_get(lua_State *L, int obj, int idx)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[48];
    switch (idx) {
    case 0: lua_pushinteger(L, (esp_timer_get_time() - s_boot_us) / 1000000); return true; // Uptime
    case 2: lua_pushinteger(L, s_state); return true;                                       // State
    case 3: lua_pushinteger(L, 1); return true;                                             // IsLocalPower
    case 5:
        snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        lua_pushstring(L, buf);
        return true;
    case 6:
        snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        lua_pushstring(L, buf);
        return true;
    case 7: lua_pushinteger(L, tm.tm_mday); return true;
    case 8: lua_pushinteger(L, tm.tm_mon + 1); return true;
    case 9: lua_pushinteger(L, tm.tm_year + 1900); return true;
    case 10: lua_pushinteger(L, tm.tm_wday == 0 ? 7 : tm.tm_wday); return true; // ISO weekday
    case 11: lua_pushinteger(L, tm.tm_hour); return true;
    case 12: lua_pushinteger(L, tm.tm_min); return true;
    case 13: lua_pushinteger(L, (lua_Integer)now); return true;
    case 17: lua_pushstring(L, s_cfg.fw_version ? s_cfg.fw_version : ""); return true;
    case 18: { // UseCloud (interface default 1): gates the MQTT connection
        lua_getfield(L, obj, "_f");
        lua_rawgeti(L, -1, 18);
        lua_remove(L, -2);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushinteger(L, 1);
        }
        return true;
    }
    case 19: // CloudConnection: MQTT broker connected
        lua_pushinteger(L, s_cfg.cloud_connected && s_cfg.cloud_connected() ? 1 : 0);
        return true;
    default: return false;
    }
}

/** @brief Class 0 execute: local CLU methods. */
static int clu0_execute(lua_State *L, int obj, int idx, int first, int nargs)
{
    switch (idx) {
    case 0: // AddToLog(msg)
        ESP_LOGI("clu_log", "%s", nargs > 0 ? luaL_tolstring(L, first, NULL) : "");
        return 0;
    case 1: // ClearLog
        return 0;
    case 2: { // SetDateTime(epoch)
        if (nargs > 0) {
            struct timeval tv = {.tv_sec = (time_t)luaL_checkinteger(L, first)};
            settimeofday(&tv, NULL);
        }
        return 0;
    }
    default:
        return -1;
    }
}

/**
 * @brief Class 0 set: forward to the host's clu_feature.
 *
 * Settable features are stored generically (return false); the host is told about them so it can
 * apply NTP server / time zone / DNS etc.
 */
static bool clu0_set(lua_State *L, int obj, int idx, int val)
{
    if (s_cfg.clu_feature) {
        s_cfg.clu_feature(idx, lua_type(L, val) == LUA_TSTRING ? lua_tostring(L, val) : NULL,
                          lua_isnumber(L, val) ? lua_tonumber(L, val) : 0);
    }
    return false;
}

static const clu_class_ops_t s_clu0_ops = {.get = clu0_get, .set = clu0_set, .execute = clu0_execute};

// ---------------------------------------------------------------------------
// VM lifecycle
// ---------------------------------------------------------------------------
/**
 * @brief Bytes held by the current Lua state (ud). Lua passes exact old sizes, so the sum is exact.
 */
typedef struct {
    size_t used, peak;
} lua_mem_t;
static lua_mem_t s_mem;

/**
 * @brief Lua allocator: PSRAM first, internal RAM (above a reserve) as fallback.
 *
 * Growth past CONFIG_CLU_LUA_MEM_LIMIT_KB fails, so Lua runs an emergency GC and retries.
 */
static void *psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    lua_mem_t *mem = ud;
    size_t old = ptr ? osize : 0; // ptr == NULL: osize is a type tag, not a size
    if (nsize == 0) {
        heap_caps_free(ptr);
        mem->used -= old;
        return NULL;
    }
    // Growth past the budget fails; Lua then runs an emergency GC and retries, else raises
    // "not enough memory". Shrinking must never fail (Lua relies on it).
    if (nsize > old && mem->used - old + nsize > (size_t)CONFIG_CLU_LUA_MEM_LIMIT_KB * 1024) {
        return NULL;
    }
    void *p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p && (nsize <= old || heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >
                                   nsize + (size_t)CONFIG_CLU_LUA_INTERNAL_RESERVE_KB * 1024)) {
        p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_8BIT); // PSRAM full: internal, with reserve
    }
    if (p) {
        mem->used = mem->used - old + nsize;
        if (mem->used > mem->peak) {
            mem->peak = mem->used;
        }
    }
    return p;
}

/** @brief Lua panic handler (log only). */
static int l_panic(lua_State *L)
{
    ESP_LOGE(TAG, "PANIC: %s", lua_tostring(L, -1));
    return 0;
}

/**
 * @brief Create a VM: libraries, __clu table, prelude, objects.lua, extensions, default checkAlive().
 *
 * __clu is removed from the globals afterwards; Lua code that needs it captured it while loading.
 * @return New state, NULL on failure.
 */
static lua_State *vm_new(void)
{
#if LUA_VERSION_NUM >= 505
    lua_State *L = lua_newstate(psram_alloc, &s_mem, esp_random()); // 5.5: hash seed argument
#else
    lua_State *L = lua_newstate(psram_alloc, &s_mem);
#endif
    if (!L) {
        return NULL;
    }
    lua_atpanic(L, l_panic);
    luaL_openlibs(L);

    static const luaL_Reg fns[] = {
        {"new", l_new},   {"get", l_get},       {"set", l_set},       {"execute", l_execute},
        {"log", l_log},   {"loop", l_loop},     {"init", l_init},     {"wait", l_wait},
        {"report", l_report}, {"millis", l_millis}, {"remote", l_remote}, {NULL, NULL},
    };
    luaL_newlib(L, fns);
    lua_pushstring(L, s_cfg.root);
    lua_setfield(L, -2, "root");
    lua_setglobal(L, "__clu");

    if (!load_embedded(L, prelude_lua_start, prelude_lua_end, "=prelude") ||
        !load_embedded(L, objects_lua_start, objects_lua_end, "=objects")) {
        lua_close(L);
        return NULL;
    }
    for (int i = 0; i < s_n_exts; i++) {
        lua_getglobal(L, "__clu");
        if (s_exts[i].fns) {
            luaL_setfuncs(L, s_exts[i].fns, 0);
        }
        lua_pop(L, 1);
        // A broken extension is logged and skipped: the CLU itself must still boot.
        if (s_exts[i].src && (luaL_loadbufferx(L, s_exts[i].src, s_exts[i].len, s_exts[i].name, "t") != LUA_OK ||
                              lua_pcall(L, 0, 0, 0) != LUA_OK)) {
            ESP_LOGE(TAG, "%s: %s", s_exts[i].name, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pushnil(L);
    lua_setglobal(L, "__clu");

    // Default checkAlive(); MAIN.LUA / EMERGNCY.LUA define their own.
    char code[64];
    if (s_emergency) {
        snprintf(code, sizeof code, "function checkAlive() return \"emergency\" end");
    } else {
        snprintf(code, sizeof code, "function checkAlive() return \"%08x\" end", s_cfg.sn);
    }
    (void)luaL_dostring(L, code);
    return L;
}

/** @brief True if @p p exists. */
static bool file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/**
 * @brief Run a boot script. Normal scripts never return (repeat SYSTEM.Loop() until false).
 * @return false on load/runtime error.
 */
static bool run_script(lua_State *L, const char *path)
{
    ESP_LOGI(TAG, "running %s", path);
    // Text only: precompiled bytecode is not verified by Lua and can corrupt memory.
    if (luaL_loadfilex(L, path, "t") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "%s: %s", path, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    ESP_LOGW(TAG, "%s returned", path);
    return true;
}

/** @brief Serve SYSTEM.Loop until a reload is requested (script returned or failed). */
static void idle_loop(lua_State *L)
{
    if (s_state == CLU_LUA_STARTING) {
        s_state = s_emergency ? CLU_LUA_EMERGENCY : CLU_LUA_OK;
    }
    while (!s_reload) {
        lua_getglobal(L, "SYSTEM");
        lua_getfield(L, -1, "Loop");
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            if (!s_reload) {
                ESP_LOGE(TAG, "SYSTEM.Loop: %s", lua_tostring(L, -1));
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
}

/** @brief Run the prelude's close hooks (storage flush etc.), then close the VM. */
static void vm_close(lua_State *L)
{
    lua_sethook(L, NULL, 0, 0); // reload_hook would abort the hooks / __gc finalizers at once
    clu_lua_shutdown_hooks(L);
    lua_close(L);
}

/**
 * @brief One VM lifetime, like one boot of the real CLU.
 *
 * Runs MAIN.LUA; on a fatal error renames it to BAD_M.LUA and runs EMERGNCY.LUA in a fresh VM
 * (emergency state). Returns once a reload was requested, with the VM closed.
 */
static void run_vm(void)
{
    char main_path[48], bad_path[48], emerg_path[48];
    snprintf(main_path, sizeof main_path, "%s/MAIN.LUA", s_cfg.root);
    snprintf(bad_path, sizeof bad_path, "%s/BAD_M.LUA", s_cfg.root);
    snprintf(emerg_path, sizeof emerg_path, "%s/EMERGNCY.LUA", s_cfg.root);

    s_emergency = false;
    s_state = CLU_LUA_STARTING;
    lua_State *L = vm_new();
    if (L && file_exists(main_path)) {
        if (run_script(L, main_path) && !s_reload) {
            idle_loop(L);
        }
        if (!s_reload) {
            // Fatal error: same as the CLU firmware, keep the script as bad_m.lua and go emergency.
            ESP_LOGE(TAG, "main.lua failed -> emergency mode");
            unlink(bad_path);
            rename(main_path, bad_path);
            vm_close(L);
            s_emergency = true;
            s_state = CLU_LUA_EMERGENCY;
            L = vm_new();
            if (L && file_exists(emerg_path)) {
                run_script(L, emerg_path);
            }
        }
    } else if (L) {
        ESP_LOGW(TAG, "no %s, idle", main_path);
    }
    if (!L) {
        ESP_LOGE(TAG, "cannot create Lua state");
        while (!s_reload) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return;
    }
    idle_loop(L); // returns at once if the reload unwound a script
    vm_close(L);
}

/**
 * @brief Lua task: run VMs back to back; between them reset queues and call the host on_reload.
 */
static void lua_task(void *arg)
{
    for (;;) {
        run_vm();
        ESP_LOGW(TAG, "reload: Lua VM closed");
        if (s_halt) {
            xSemaphoreGive(s_reloaded);
            vTaskSuspend(NULL); // reboot follows
        }
        s_reload = false; // a request from here on reloads the next VM again
        xQueueReset(s_events); // driver events for objects of the old VM
        drop_remote_results(); // replies for Lua threads of the old VM
        s_reload_ok = !s_cfg.on_reload || s_cfg.on_reload();
        s_boot_us = esp_timer_get_time(); // Uptime restarts as after a reboot
        xSemaphoreGive(s_reloaded);
        if (!s_reload_ok) {
            ESP_LOGE(TAG, "reload: host reset failed, waiting for reboot");
            vTaskSuspend(NULL);
        }
    }
}

bool clu_lua_reload(int timeout_ms)
{
    if (!s_reloaded) {
        return false;
    }
    xSemaphoreTake(s_reloaded, 0); // drop a stale completion
    ESP_LOGW(TAG, "reload requested");
    s_reload = true;
    if (xSemaphoreTake(s_reloaded, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "reload: script did not reach SYSTEM.Loop in %d ms", timeout_ms);
        return false;
    }
    return s_reload_ok;
}

bool clu_lua_halt(int timeout_ms)
{
    s_halt = true;
    if (!s_reloaded) {
        return false;
    }
    xSemaphoreTake(s_reloaded, 0);
    s_reload = true;
    return xSemaphoreTake(s_reloaded, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void clu_lua_shutdown_hooks(lua_State *L)
{
    int top = lua_gettop(L);
    if (lua_getglobal(L, "__shutdown") == LUA_TFUNCTION && lua_pcall(L, 0, 0, 0) != LUA_OK) {
        ESP_LOGW(TAG, "shutdown: %s", lua_tostring(L, -1));
    }
    lua_settop(L, top);
}

void clu_lua_start(const clu_lua_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_boot_us = esp_timer_get_time();
    s_reqs = xQueueCreate(16, sizeof(req_t *));
    s_events = xQueueCreate(64, sizeof(event_t));
    s_remote_res = xQueueCreate(16, sizeof(remote_res_t));
    s_reloaded = xSemaphoreCreateBinary();
    clu_lua_register_class(0, &s_clu0_ops);
    xTaskCreatePinnedToCore(lua_task, "clu_lua", LUA_STACK, NULL, 5, NULL, 1);
}
