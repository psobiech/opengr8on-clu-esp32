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
static bool s_soft_reloaded;        // a VM was replaced by clu_lua_reload() since the chip boot

#define MAX_EXTS 6
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
    char str[16]; // string value (is_value, str[0] != 0): e.g. LEDRGB "#RRGGBB"
} event_t;

/** @brief Result of a remote CLU call. */
typedef struct {
    uint32_t id;
    char *result; // heap; NULL = timeout / send failure
} remote_res_t;

/** @brief Free a request and its buffers. */
static void req_free(req_t *request)
{
    vSemaphoreDelete(request->done);
    free(request->expr);
    free(request->out);
    free(request);
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
    req_t *request = calloc(1, sizeof *request);
    if (!request) {
        return ESP_ERR_NO_MEM;
    }
    request->expr = strdup(expr);
    request->out = malloc(cap);
    request->cap = cap;
    request->done = xSemaphoreCreateBinary();
    strlcpy(request->from, from_ip ? from_ip : "", sizeof request->from);
    if (!request->expr || !request->out || !request->done) {
        if (request->done) {
            vSemaphoreDelete(request->done);
        }
        free(request->expr);
        free(request->out);
        free(request);
        return ESP_ERR_NO_MEM;
    }
    request->out[0] = 0;
    if (xQueueSend(s_reqs, &request, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        req_free(request);
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(request->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        strlcpy(out, request->out, cap);
        req_free(request);
        return ESP_OK;
    }
    int prev = __atomic_exchange_n(&request->state, REQ_ABANDONED, __ATOMIC_ACQ_REL);
    if (prev == REQ_DONE) {
        // Finished just after our timeout: result is valid.
        xSemaphoreTake(request->done, portMAX_DELAY);
        strlcpy(out, request->out, cap);
        req_free(request);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT; // Lua task frees it
}

void clu_lua_post_event(const char *obj_name, int event_idx)
{
    if (!s_events) {
        return;
    }
    event_t event_data = {.idx = event_idx};
    strlcpy(event_data.name, obj_name, sizeof event_data.name);
    xQueueSend(s_events, &event_data, 0);
}

void clu_lua_post_number(const char *obj_name, int feature_idx, double value)
{
    if (!s_events) {
        return;
    }
    event_t event_data = {.idx = feature_idx, .is_value = true, .value = value};
    strlcpy(event_data.name, obj_name, sizeof event_data.name);
    xQueueSend(s_events, &event_data, 0);
}

void clu_lua_post_string(const char *obj_name, int feature_idx, const char *value)
{
    if (!s_events) {
        return;
    }
    event_t event_data = {.idx = feature_idx, .is_value = true};
    strlcpy(event_data.name, obj_name, sizeof event_data.name);
    strlcpy(event_data.str, value, sizeof event_data.str);
    xQueueSend(s_events, &event_data, 0);
}

void clu_lua_post_remote(uint32_t id, const char *result)
{
    if (!s_remote_res) {
        return;
    }
    remote_res_t remote_response = {.id = id, .result = result ? strdup(result) : NULL};
    if (xQueueSend(s_remote_res, &remote_response, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "remote result %lu dropped (queue full)", (unsigned long)id);
        free(remote_response.result);
    }
}

/** @brief Discard queued remote-call results (their Lua threads died with the old VM). */
static void drop_remote_results(void)
{
    remote_res_t remote_response;
    while (xQueueReceive(s_remote_res, &remote_response, 0) == pdTRUE) {
        free(remote_response.result);
    }
}

// ---------------------------------------------------------------------------
// Object helpers
// ---------------------------------------------------------------------------
const char *clu_obj_name(lua_State *L, int obj)
{
    lua_getfield(L, obj, "_name");
    const char *object_name = lua_tostring(L, -1); // string stays referenced by the object table
    lua_pop(L, 1);
    return object_name ? object_name : "";
}

int clu_obj_class(lua_State *L, int obj)
{
    lua_getfield(L, obj, "_class");
    int object_class = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return object_class;
}

/** @brief Class hooks of the object at @p obj, NULL if none registered. */
static const clu_class_ops_t *ops_for(lua_State *L, int obj)
{
    int object_class = clu_obj_class(L, obj);
    return (object_class >= 0 && object_class < MAX_CLASSES) ? s_classes[object_class] : NULL;
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
    int result_count = ops && ops->execute ? ops->execute(L, 1, idx, 3, nargs) : -1;
    if (result_count < 0) {
        lua_settop(L, top);
        ESP_LOGD(TAG, "%s:execute(%d) not implemented (class %d)", clu_obj_name(L, 1), idx,
                 clu_obj_class(L, 1));
        return 0;
    }
    return result_count;
}

/** @brief Lua: `__clu.log(level, ...)` — ESP_LOG the arguments. */
static int l_log(lua_State *L)
{
    int level = (int)luaL_checkinteger(L, 1);
    luaL_Buffer log_buffer;
    luaL_buffinit(L, &log_buffer);
    int stack_top = lua_gettop(L);
    for (int i = 2; i <= stack_top; i++) {
        if (i > 2) {
            luaL_addchar(&log_buffer, '\t');
        }
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&log_buffer);
    }
    luaL_pushresult(&log_buffer);
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
static void req_hook(lua_State *L, lua_Debug *debug_info)
{
    (void)debug_info;
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
    req_t *request = lua_touserdata(L, 1);
    size_t expression_length = strlen(request->expr);

    lua_pushstring(L, request->from);
    lua_setglobal(L, "__req_from");

    lua_pushliteral(L, "return ");
    lua_pushstring(L, request->expr);
    lua_concat(L, 2);
    size_t code_length;
    const char *code = lua_tolstring(L, -1, &code_length);
    int load_status = luaL_loadbufferx(L, code, code_length, "=req", "t");
    lua_remove(L, -2);
    if (load_status != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadbufferx(L, request->expr, expression_length, "=req", "t") != LUA_OK) { // statement, not expression
            return lua_error(L);
        }
    }
    int base = lua_gettop(L) - 1;
    lua_call(L, 0, LUA_MULTRET);
    if (lua_gettop(L) > base) {
        strlcpy(request->out, luaL_tolstring(L, base + 1, NULL), request->cap);
    } else {
        strlcpy(request->out, "nil", request->cap);
    }
    return 0;
}

/**
 * @brief Execute one req: call with the time-limit hook, store the result and hand the request back.
 */
static void exec_request(lua_State *L, req_t *request)
{
    size_t expression_length = strlen(request->expr);
    while (expression_length && (request->expr[expression_length - 1] == '\r' || request->expr[expression_length - 1] == '\n')) {
        request->expr[--expression_length] = 0;
    }

    // A GATE answers checkAlive() itself with its serial in hex (observed on GATE ALARM / MODBUS):
    // its MAIN.LUA defines checkAlive() as print("true"), which returns nothing.
    if (s_cfg.gate && strcmp(request->expr, "checkAlive()") == 0) {
        if (s_emergency) {
            strlcpy(request->out, "emergency", request->cap);
        } else {
            snprintf(request->out, request->cap, "%08x", s_cfg.sn);
        }
        goto done;
    }

    // Requests run nested in SYSTEM.Loop (MAIN.LUA): keep the caller's hook state.
    lua_Hook prev_hook = lua_gethook(L);
    int prev_mask = lua_gethookmask(L), prev_count = lua_gethookcount(L);
    s_req_deadline_us = esp_timer_get_time() + (int64_t)REQ_BUDGET_MS * 1000;
    lua_sethook(L, req_hook, LUA_MASKCOUNT, REQ_HOOK_INSNS);
    lua_pushcfunction(L, req_protected);
    lua_pushlightuserdata(L, request);
    s_req_active = true;
    int call_status = lua_pcall(L, 1, 0, 0);
    s_req_active = false;
    lua_sethook(L, prev_hook, prev_mask, prev_count);
    if (call_status != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        ESP_LOGW(TAG, "req '%s' failed: %s", request->expr, err ? err : "(non-string error)");
        lua_pop(L, 1);
        strlcpy(request->out, "nil", request->cap);
    }

done:;
    int prev = __atomic_exchange_n(&request->state, REQ_DONE, __ATOMIC_ACQ_REL);
    if (prev == REQ_ABANDONED) {
        req_free(request);
    } else {
        xSemaphoreGive(request->done);
    }
}

/** @brief lua_pcall body: `__remote_done(id, result)` for the ::remote_res_t (light userdata at 1). */
static int remote_done_p(lua_State *L)
{
    const remote_res_t *remote_response = lua_touserdata(L, 1);
    lua_getglobal(L, "__remote_done"); // resumes the Lua thread waiting for this call
    lua_pushinteger(L, remote_response->id);
    if (remote_response->result) {
        lua_pushstring(L, remote_response->result);
    } else {
        lua_pushnil(L);
    }
    lua_call(L, 2, 0);
    return 0;
}

/** @brief Deliver queued driver events / values to Lua (__fire / __update). */
static void drain_events(lua_State *L)
{
    event_t event_data;
    while (xQueueReceive(s_events, &event_data, 0) == pdTRUE) {
        lua_getglobal(L, event_data.is_value ? "__update" : "__fire");
        lua_pushstring(L, event_data.name);
        lua_pushinteger(L, event_data.idx);
        int lua_argument_count = 2;
        if (event_data.is_value) {
            if (event_data.str[0]) {
                lua_pushstring(L, event_data.str);
            } else if (event_data.value == (double)(lua_Integer)event_data.value) {
                lua_pushinteger(L, (lua_Integer)event_data.value);
            } else {
                lua_pushnumber(L, event_data.value);
            }
            lua_argument_count = 3;
        }
        if (lua_pcall(L, lua_argument_count, 0, 0) != LUA_OK) {
            ESP_LOGW(TAG, "event %s/%d: %s", event_data.name, event_data.idx, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    remote_res_t remote_response;
    while (xQueueReceive(s_remote_res, &remote_response, 0) == pdTRUE) {
        // Everything that can raise (pushing the result string too) runs under the pcall, so
        // r.result is freed even on out of memory.
        lua_pushcfunction(L, remote_done_p);
        lua_pushlightuserdata(L, &remote_response);
        int call_status = lua_pcall(L, 1, 0, 0);
        free(remote_response.result);
        if (call_status != LUA_OK) {
            ESP_LOGW(TAG, "remote result %lu: %s", (unsigned long)remote_response.id, lua_tostring(L, -1));
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
static void reload_hook(lua_State *L, lua_Debug *debug_info)
{
    (void)debug_info;
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
    req_t *request;
    if (xQueueReceive(s_reqs, &request, pdMS_TO_TICKS(LOOP_WAIT_MS)) == pdTRUE) {
        exec_request(L, request);
        for (int i = 1; i < REQS_PER_LOOP && xQueueReceive(s_reqs, &request, 0) == pdTRUE; i++) {
            exec_request(L, request);
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
        unsigned first_octet, second_octet, third_octet, fourth_octet;
        if (sscanf(luaL_checkstring(L, 1), "%u.%u.%u.%u", &first_octet, &second_octet, &third_octet, &fourth_octet) != 4 || first_octet > 255 || second_octet > 255 ||
            third_octet > 255 || fourth_octet > 255) {
            return luaL_error(L, "bad remote CLU address");
        }
        ip = first_octet << 24 | second_octet << 16 | third_octet << 8 | fourth_octet;
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
// Class 0: local CLU (clu_ZWAVE_2_ft00000003_fv203 interface) or GATE
// (clu_GATE_HTTP_ft00000003_fv00000578 interface, s_cfg.gate)
// ---------------------------------------------------------------------------
/** @brief Push the stored value of feature @p idx of the object at @p obj, @p def if unset. */
static void push_stored(lua_State *L, int obj, int idx, lua_Integer def)
{
    lua_getfield(L, obj, "_f");
    lua_rawgeti(L, -1, idx);
    lua_remove(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushinteger(L, def);
    }
}

/**
 * @brief GATE class 0 get: Uptime 0, Date 5, Time 6, LocalTime 13, UnixTime 15, FirmwareVersion 17,
 *        UseCloud 18, CloudConnection 19, ResetReason 26. The settable rest (ClientReportInterval 1,
 *        DNS 2/3, TimeZone 14, NTPTimeout 20, UseNTP 21, TelnetLogLevel 23, ...) is stored.
 */
static bool gate0_get(lua_State *L, int obj, int idx, time_t now, const struct tm *local_time)
{
    char buf[48];
    switch (idx) {
    case 0: lua_pushinteger(L, (esp_timer_get_time() - s_boot_us) / 1000000); return true; // Uptime
    case 5:
        snprintf(buf, sizeof buf, "%04d-%02d-%02d", local_time->tm_year + 1900, local_time->tm_mon + 1, local_time->tm_mday);
        lua_pushstring(L, buf);
        return true;
    case 6:
        snprintf(buf, sizeof buf, "%02d:%02d:%02d", local_time->tm_hour, local_time->tm_min, local_time->tm_sec);
        lua_pushstring(L, buf);
        return true;
    case 13: lua_pushinteger(L, (lua_Integer)now); return true; // LocalTime (as CLU feature 13)
    case 15: lua_pushinteger(L, (lua_Integer)now); return true; // UnixTime
    case 17: lua_pushstring(L, s_cfg.fw_version ? s_cfg.fw_version : ""); return true;
    case 18: push_stored(L, obj, 18, 1); return true; // UseCloud (interface default 1): MQTT on/off
    case 19: lua_pushinteger(L, s_cfg.cloud_connected && s_cfg.cloud_connected() ? 1 : 0); return true;
    case 26: lua_pushinteger(L, s_soft_reloaded ? 2 : s_cfg.reset_reason); return true; // ResetReason
    default: return false;
    }
}

/**
 * @brief Class 0 get: computed features (Uptime, State, date/time, FwVersion, UseCloud, CloudConnection,
 *        LastMeasurementSendTime).
 */
static bool clu0_get(lua_State *L, int obj, int idx)
{
    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    if (s_cfg.gate) {
        return gate0_get(L, obj, idx, now, &local_time);
    }
    char buf[48];
    switch (idx) {
    case 0: lua_pushinteger(L, (esp_timer_get_time() - s_boot_us) / 1000000); return true; // Uptime
    case 2: lua_pushinteger(L, s_state); return true;                                       // State
    case 3: lua_pushinteger(L, 1); return true;                                             // IsLocalPower
    case 5:
        snprintf(buf, sizeof buf, "%04d-%02d-%02d", local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday);
        lua_pushstring(L, buf);
        return true;
    case 6:
        snprintf(buf, sizeof buf, "%02d:%02d:%02d", local_time.tm_hour, local_time.tm_min, local_time.tm_sec);
        lua_pushstring(L, buf);
        return true;
    case 7: lua_pushinteger(L, local_time.tm_mday); return true;
    case 8: lua_pushinteger(L, local_time.tm_mon + 1); return true;
    case 9: lua_pushinteger(L, local_time.tm_year + 1900); return true;
    case 10: lua_pushinteger(L, local_time.tm_wday == 0 ? 7 : local_time.tm_wday); return true; // ISO weekday
    case 11: lua_pushinteger(L, local_time.tm_hour); return true;
    case 12: lua_pushinteger(L, local_time.tm_min); return true;
    case 13: lua_pushinteger(L, (lua_Integer)now); return true;
    case 17: lua_pushstring(L, s_cfg.fw_version ? s_cfg.fw_version : ""); return true;
    case 18: push_stored(L, obj, 18, 1); return true; // UseCloud (interface default 1): MQTT on/off
    case 19: // CloudConnection: MQTT broker connected
        lua_pushinteger(L, s_cfg.cloud_connected && s_cfg.cloud_connected() ? 1 : 0);
        return true;
    case 35: lua_pushstring(L, ""); return true; // LastMeasurementSendTime: no measurements sent
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
            struct timeval time_value = {.tv_sec = (time_t)luaL_checkinteger(L, first)};
            settimeofday(&time_value, NULL);
            if (s_cfg.on_time_set) {
                s_cfg.on_time_set();
            }
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
    // GATE: UseCloud = true switches UseNTP (21) on as well (GATE HTTP manual, NTP section).
    if (s_cfg.gate && idx == 18 &&
        (lua_isboolean(L, val) ? lua_toboolean(L, val) : lua_isnumber(L, val) && lua_tonumber(L, val) != 0)) {
        lua_getfield(L, obj, "_f");
        lua_pushinteger(L, 1);
        lua_rawseti(L, -2, 21);
        lua_pop(L, 1);
        if (s_cfg.clu_feature) {
            s_cfg.clu_feature(21, NULL, 1);
        }
    }
    return false;
}

static const clu_class_ops_t s_clu0_ops = {.get = clu0_get, .set = clu0_set, .execute = clu0_execute};

// ---------------------------------------------------------------------------
// VM lifecycle
// ---------------------------------------------------------------------------
/**
 * @brief Bytes held by the current Lua state (user_data). Lua passes exact old sizes, so the sum is exact.
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
static void *psram_alloc(void *user_data, void *ptr, size_t osize, size_t nsize)
{
    lua_mem_t *mem = user_data;
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
    void *allocation = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!allocation && (nsize <= old || heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >
                                   nsize + (size_t)CONFIG_CLU_LUA_INTERNAL_RESERVE_KB * 1024)) {
        allocation = heap_caps_realloc(ptr, nsize, MALLOC_CAP_8BIT); // PSRAM full: internal, with reserve
    }
    if (allocation) {
        mem->used = mem->used - old + nsize;
        if (mem->used > mem->peak) {
            mem->peak = mem->used;
        }
    }
    return allocation;
}

/** @brief Lua panic handler (log only). */
static int l_panic(lua_State *L)
{
    ESP_LOGE(TAG, "PANIC: %s", lua_tostring(L, -1));
    return 0;
}

/**
 * @brief lua_pcall body of vm_new(): standard libraries and the private __clu table with the
 *        extensions' C functions.
 */
static int vm_setup(lua_State *L)
{
    luaL_openlibs(L);
    static const luaL_Reg fns[] = {
        {"new", l_new},   {"get", l_get},       {"set", l_set},       {"execute", l_execute},
        {"log", l_log},   {"loop", l_loop},     {"init", l_init},     {"wait", l_wait},
        {"report", l_report}, {"millis", l_millis}, {"remote", l_remote}, {NULL, NULL},
    };
    luaL_newlib(L, fns);
    lua_pushstring(L, s_cfg.root);
    lua_setfield(L, -2, "root");
    lua_pushinteger(L, s_cfg.sn);
    lua_setfield(L, -2, "sn");
    lua_pushboolean(L, s_cfg.gate);
    lua_setfield(L, -2, "gate");
    for (int i = 0; i < s_n_exts; i++) {
        if (s_exts[i].fns) {
            luaL_setfuncs(L, s_exts[i].fns, 0);
        }
    }
    lua_setglobal(L, "__clu");
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
    // Out of memory while setting up must not reach the panic handler (abort -> reboot loop).
    lua_pushcfunction(L, vm_setup);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Lua VM setup: %s (internal RAM free %u B, largest block %u B, reserve %u KB, PSRAM %u B)",
                 lua_tostring(L, -1), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)CONFIG_CLU_LUA_INTERNAL_RESERVE_KB, (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
        lua_close(L);
        return NULL;
    }

    if (!load_embedded(L, prelude_lua_start, prelude_lua_end, "=prelude") ||
        !load_embedded(L, objects_lua_start, objects_lua_end, "=objects")) {
        lua_close(L);
        return NULL;
    }
    for (int i = 0; i < s_n_exts; i++) {
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
static bool file_exists(const char *file_path)
{
    struct stat file_status;
    return stat(file_path, &file_status) == 0 && S_ISREG(file_status.st_mode);
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
        if (s_reload) {
            ESP_LOGI(TAG, "%s stopped for the reload", path); // unwound by l_loop on purpose
        } else {
            ESP_LOGE(TAG, "%s: %s", path, lua_tostring(L, -1));
        }
        lua_pop(L, 1);
        return false;
    }
    if (s_cfg.gate) {
        // A GATE's MAIN.LUA ends after SYSTEM.Init(); the firmware runs SYSTEM.Loop (idle_loop).
        ESP_LOGI(TAG, "%s done, the firmware runs SYSTEM.Loop", path);
    } else {
        ESP_LOGW(TAG, "%s returned", path);
    }
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
        s_soft_reloaded = true;           // GATE ResetReason 2 (configuration reload)
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
