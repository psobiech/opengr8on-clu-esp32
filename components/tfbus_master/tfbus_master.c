/**
 * @file tfbus_master.c
 * @brief TFBUS bus master: module registry, Lua class hooks, discovery and the polling bus task.
 *
 * Threads: the Lua task declares modules / IO objects (class hooks) and calls drivers' io_* hooks
 * under s_drv_lock; the bus task owns the UART, runs discovery, polls modules and runs deferred
 * driver work. Commands from any task go through the s_cmds queue.
 */
#include "tfbus_master.h"

#include <string.h>

#include "clu_lua.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "tfbus_uart.h"

static const char *TAG = "tfbus";

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
static TaskHandle_t s_task;
static volatile bool s_restart;
static volatile bool s_refresh; // next discovery was requested by req_refresh_modules
// Bus task idle and touching no module state until tfbus_master_start() (boot, Lua reload).
static volatile bool s_park = true;
static volatile bool s_parked;
static const tfbus_driver_t *s_drivers[MAX_DRIVERS];
static int s_ndrivers;

// Modules found on the bus (CONFIG.JSON "tfbusDevices"): OM.LUA modules that answered plus new
// ones reported by DISCOVER. Rebuilt by every discovery; read under s_lock.
static tfbus_device_t s_dev[TFBUS_MAX_MODULES];
static int s_ndev;
static SemaphoreHandle_t s_scan_done;
static void (*s_scan_cb)(void);

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

tfbus_module_t *tfbus_master_get(int i)
{
    return (i >= 0 && i < s_nmod) ? &s_mod[i] : NULL;
}

tfbus_module_t *tfbus_master_find_sn(uint32_t sn)
{
    for (int i = 0; i < s_nmod; i++) {
        if (s_mod[i].sn == sn) {
            return &s_mod[i];
        }
    }
    return NULL;
}

tfbus_module_t *tfbus_master_find_name(const char *name)
{
    for (int i = 0; i < s_nmod; i++) {
        if (strcmp(s_mod[i].name, name) == 0) {
            return &s_mod[i];
        }
    }
    return NULL;
}

static void drain_cmds(void);

esp_err_t tfbus_master_queue(const uint8_t *frame, size_t len)
{
    if (!s_cmds || len > TFBUS_MAX_FRAME) {
        return ESP_ERR_INVALID_ARG;
    }
    cmd_t c = {.len = (uint8_t)len};
    memcpy(c.frame, frame, len);
    if (xTaskGetCurrentTaskHandle() == s_task) {
        // Bus task (driver init / on_status): never block on its own queue. A full queue is
        // flushed to the bus right here (SMART_PANEL init alone is ~275 frames per panel).
        if (xQueueSend(s_cmds, &c, 0) == pdTRUE) {
            return ESP_OK;
        }
        drain_cmds();
        return xQueueSend(s_cmds, &c, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    return xQueueSend(s_cmds, &c, pdMS_TO_TICKS(50)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool tfbus_master_defer(tfbus_defer_fn_t fn, tfbus_module_t *m, void *arg, uint32_t tag)
{
    defer_t d = {.fn = fn, .m = m, .arg = arg, .tag = tag};
    if (!s_defer || xQueueSend(s_defer, &d, 0) != pdTRUE) {
        ESP_LOGW(TAG, "defer queue full, call dropped");
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
    defer_t d;
    while (xQueueReceive(s_defer, &d, 0) == pdTRUE) {
        d.fn(d.m, d.arg, d.tag);
    }
    xSemaphoreGive(s_drv_lock);
}

// ---------------------------------------------------------------------------
// Lua class 2: OBJECT:new(2, sn, hwType, "snhex")
// ---------------------------------------------------------------------------
/**
 * @brief Lua class 2 hook: OBJECT:new(2, sn, hwType, "snhex") declares a bus module.
 *
 * Assigns the next bus address in declaration order and stores it as obj._addr. Re-declaring an
 * existing SN only follows the new Lua name. Non-TFBUS types (> 0xFF, e.g. Z-Wave) are skipped.
 */
static void module_on_new(lua_State *L, int obj)
{
    lua_getfield(L, obj, "_sn");
    lua_Integer sn = lua_tointeger(L, -1);
    lua_getfield(L, obj, "_hwType");
    lua_Integer hw = lua_tointeger(L, -1);
    lua_pop(L, 2);

    if (hw <= 0 || hw > 0xFF) {
        // e.g. 0x008500030011 = Z-Wave node, not on the RS485 bus (skipped by the real CLU too)
        ESP_LOGI(TAG, "skip non-TFBUS module %s (type 0x%llx)", clu_obj_name(L, obj), (long long)hw);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_nmod; i++) {
        tfbus_module_t *d = &s_mod[i];
        if (d->sn == (uint32_t)sn) {
            // Declared again (e.g. OBJECT:new(2, ...) via req:): keep the bus entry (address,
            // driver state), only follow the new Lua name. No new address, no leaked slot.
            if (d->hw_type != (uint8_t)hw) {
                ESP_LOGW(TAG, "module sn=%lu redeclared with hw 0x%02x (was 0x%02x), keeping old",
                         (unsigned long)d->sn, (unsigned)hw, d->hw_type);
            }
            strlcpy(d->name, clu_obj_name(L, obj), sizeof d->name);
            uint8_t addr = d->addr;
            xSemaphoreGive(s_lock);
            lua_pushinteger(L, addr);
            lua_setfield(L, obj, "_addr");
            ESP_LOGI(TAG, "module sn=%lu redeclared, addr=%u", (unsigned long)sn, addr);
            return;
        }
    }
    if (s_nmod >= TFBUS_MAX_MODULES) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "too many modules");
        return;
    }
    tfbus_module_t *m = &s_mod[s_nmod];
    memset(m, 0, sizeof *m);
    m->sn = (uint32_t)sn;
    m->hw_type = (uint8_t)hw;
    m->addr = (uint8_t)(s_nmod + 1);
    strlcpy(m->name, clu_obj_name(L, obj), sizeof m->name);
    m->drv = driver_for(m->hw_type);
    s_nmod++;
    xSemaphoreGive(s_lock);

    lua_pushinteger(L, m->addr);
    lua_setfield(L, obj, "_addr");
    ESP_LOGI(TAG, "module %u sn=%lu hw=0x%02x addr=%u driver=%s", s_nmod, (unsigned long)m->sn,
             m->hw_type, m->addr, m->drv ? m->drv->name : "-");
}

static const clu_class_ops_t s_module_ops = {.on_new = module_on_new};

// ---------------------------------------------------------------------------
// IO objects: OBJECT:new(class, moduleObj, channel, name) -> module driver
// ---------------------------------------------------------------------------
void tfbus_obj_store(lua_State *L, int obj, int idx, lua_Number v)
{
    lua_getfield(L, obj, "_f");
    if (v == (lua_Number)(lua_Integer)v) {
        lua_pushinteger(L, (lua_Integer)v);
    } else {
        lua_pushnumber(L, v);
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
static tfbus_module_t *io_module(lua_State *L, int obj, int *ch)
{
    lua_getfield(L, obj, "_module");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return NULL;
    }
    lua_getfield(L, -1, "_sn");
    tfbus_module_t *m = tfbus_master_find_sn((uint32_t)lua_tointeger(L, -1));
    lua_pop(L, 2);
    lua_getfield(L, obj, "_channel");
    *ch = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return (m && m->drv) ? m : NULL;
}

/** @brief IO class hook on_new: forward to the driver's io_new. */
static void io_on_new(lua_State *L, int obj)
{
    int ch;
    tfbus_module_t *m = io_module(L, obj, &ch);
    if (m && m->drv->io_new) {
        xSemaphoreTake(s_drv_lock, portMAX_DELAY);
        m->drv->io_new(m, clu_obj_class(L, obj), ch, clu_obj_name(L, obj));
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
    io_call_t *c = lua_touserdata(L, 1);
    if (c->execute) {
        c->ret = c->m->drv->io_execute(c->m, L, 2, c->cls, c->ch, c->idx, 3, c->nargs);
        return c->ret > 0 ? c->ret : 0;
    }
    c->ret = c->m->drv->io_set(c->m, L, 2, c->cls, c->ch, c->idx, 3);
    return 0;
}

/**
 * @brief Run a driver io hook in a protected call under s_drv_lock; re-raise its error after unlocking.
 * @return ::io_call_t::ret.
 */
static int io_call(lua_State *L, io_call_t *c, int obj, int first)
{
    lua_pushcfunction(L, io_call_p);
    lua_pushlightuserdata(L, c);
    lua_pushvalue(L, obj);
    for (int i = 0; i < c->nargs; i++) {
        lua_pushvalue(L, first + i);
    }
    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    int st = lua_pcall(L, 2 + c->nargs, LUA_MULTRET, 0);
    xSemaphoreGive(s_drv_lock);
    if (st != LUA_OK) {
        lua_error(L); // error object on top
    }
    return c->ret;
}

/** @brief IO class hook set: forward to the driver's io_set. */
static bool io_set(lua_State *L, int obj, int idx, int val)
{
    int ch;
    tfbus_module_t *m = io_module(L, obj, &ch);
    if (!m || !m->drv->io_set) {
        return false;
    }
    io_call_t c = {.m = m, .cls = clu_obj_class(L, obj), .ch = ch, .idx = idx, .nargs = 1};
    int top = lua_gettop(L);
    io_call(L, &c, obj, val);
    lua_settop(L, top);
    return c.ret != 0;
}

/** @brief IO class hook execute: forward to the driver's io_execute. */
static int io_execute(lua_State *L, int obj, int idx, int first, int nargs)
{
    int ch;
    tfbus_module_t *m = io_module(L, obj, &ch);
    if (!m || !m->drv->io_execute) {
        return -1;
    }
    io_call_t c = {.m = m, .cls = clu_obj_class(L, obj), .ch = ch, .idx = idx, .nargs = nargs,
                   .execute = true};
    int top = lua_gettop(L);
    io_call(L, &c, obj, first);
    if (c.ret <= 0) {
        lua_settop(L, top);
    }
    return c.ret; // results (if any) are on top
}

static const clu_class_ops_t s_io_ops = {.on_new = io_on_new, .set = io_set, .execute = io_execute};

// ---------------------------------------------------------------------------
// Bus task
// ---------------------------------------------------------------------------
/** @brief True if the reply is an empty ACK frame (AA 00 01 xor). */
static bool is_ack(const uint8_t *f, int n)
{
    return n == 4 && f[0] == 0xAA && f[1] == 0x00 && f[2] == 0x01;
}

/**
 * @brief Send one queued frame: broadcasts fire-and-forget, others retried up to CMD_TRIES until answered.
 */
static void send_cmd(const cmd_t *c)
{
    uint8_t rx[TFBUS_MAX_FRAME];
    if (c->frame[1] == TFBUS_BROADCAST && c->frame[2] != TFBUS_T_ADDR_ASSIGN &&
        c->frame[2] != TFBUS_T_SEEK) {
        tfbus_uart_send(c->frame, c->len);
        return;
    }
    for (int t = 0; t < CMD_TRIES; t++) {
        int n = tfbus_uart_transact(c->frame, c->len, rx, sizeof rx, ACK_TIMEOUT_MS);
        if (n > 0) {
            return; // ACK (or any reply)
        }
    }
    ESP_LOGW(TAG, "no ACK for cmd type 0x%02x to addr %u", c->frame[2], c->frame[1]);
}

/** @brief Send every queued command now (bus task only). */
static void drain_cmds(void)
{
    cmd_t c;
    while (xQueueReceive(s_cmds, &c, 0) == pdTRUE) {
        send_cmd(&c);
    }
}

/** @brief Update the online flag and notify the driver on change. */
static void set_online(tfbus_module_t *m, bool on)
{
    if (m->online == on) {
        return;
    }
    m->online = on;
    ESP_LOGW(TAG, "addr %u sn=%lu %s", m->addr, (unsigned long)m->sn, on ? "online" : "OFFLINE");
    if (m->drv && m->drv->on_online) {
        m->drv->on_online(m, on);
    }
}

/** @brief ADDR_ASSIGN the module's SN to its address; true if ACKed (up to CMD_TRIES). */
static bool assign(tfbus_module_t *m)
{
    uint8_t f[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];
    size_t n = tfbus_addr_assign(f, sizeof f, m->sn, m->addr);
    m->assigned = false;
    for (int t = 0; t < CMD_TRIES && !m->assigned; t++) {
        int r = tfbus_uart_transact(f, n, rx, sizeof rx, ACK_TIMEOUT_MS);
        m->assigned = is_ack(rx, r);
    }
    return m->assigned;
}

static void poll_module(tfbus_module_t *m);

/** @brief Find or append the s_dev entry for @p sn; NULL if the table is full. */
static tfbus_device_t *dev_get(uint32_t sn)
{
    for (int i = 0; i < s_ndev; i++) {
        if (s_dev[i].sn == sn) {
            return &s_dev[i];
        }
    }
    if (s_ndev >= TFBUS_MAX_MODULES) {
        return NULL;
    }
    tfbus_device_t *d = &s_dev[s_ndev++];
    memset(d, 0, sizeof *d);
    d->sn = sn;
    d->hw_ver = 1; // real CLU default when VERSION_QUERY has no 4th byte / is not sent
    return d;
}

/**
 * @brief DISCOVER loop (broadcast type 0x08).
 *
 * Every still-unaddressed module answers SN hwType fwType fwApiVer; the CLU addresses it (OM.LUA
 * address if known, else the next free one) so the next DISCOVER reaches the remaining ones.
 * Ends on silence or an empty ACK ("SEEK - no data - finish").
 */
static void scan(void)
{
    uint8_t f[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];
    size_t n = tfbus_discover(f, sizeof f);
    uint8_t next_addr = (uint8_t)(s_nmod + 1);
    int garbled = 0;
    for (int found = 0; found < TFBUS_MAX_MODULES;) {
        int r = tfbus_uart_transact(f, n, rx, sizeof rx, DISCOVER_TIMEOUT_MS);
        const uint8_t *pl;
        size_t plen;
        tfbus_ident_t id;
        if (r <= 0) {
            break;
        }
        if (!tfbus_parse_response(rx, (size_t)r, &pl, &plen)) {
            if (++garbled > DISCOVER_RETRIES) { // several modules answering at once
                ESP_LOGW(TAG, "DISCOVER: garbled replies, giving up");
                break;
            }
            continue;
        }
        if (plen == 0 || !tfbus_parse_discover(pl, plen, &id)) {
            break; // plain ACK: no (more) unaddressed modules
        }
        found++;
        tfbus_module_t *m = tfbus_master_find_sn(id.sn);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        tfbus_device_t *d = dev_get(id.sn);
        if (d) {
            d->hw_type = id.hw_type;
            d->fw_type = id.fw_type;
            d->fw_api_ver = id.fw_api_ver;
            d->known = m != NULL;
        }
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "DISCOVER: sn=%08lx hw=0x%02x fwType=%u fwApiVer=%u%s", (unsigned long)id.sn,
                 id.hw_type, id.fw_type, id.fw_api_ver, m ? "" : " (not in OM.LUA)");
        bool ok;
        if (m) {
            ok = assign(m);
        } else if (next_addr <= TFBUS_MAX_MODULES) {
            tfbus_module_t tmp = {.sn = id.sn, .addr = next_addr++};
            ok = assign(&tmp);
            if (d && ok) {
                d->addr = tmp.addr;
            }
        } else {
            ESP_LOGW(TAG, "DISCOVER: no free address");
            break;
        }
        if (!ok) {
            break; // it would answer the next DISCOVER again
        }
    }
}

/**
 * @brief Identity of an addressed OM.LUA module that DISCOVER did not report.
 *
 * Taken from the previous scan's entry (prev, may be NULL), else from what OM.LUA says. Under s_lock.
 */
static void dev_fill(tfbus_device_t *d, const tfbus_module_t *m, const tfbus_device_t *prev, int nprev)
{
    const tfbus_device_t *p = NULL;
    for (int j = 0; j < nprev && !p; j++) {
        p = prev[j].sn == m->sn && prev[j].hw_type ? &prev[j] : NULL;
    }
    if (p) {
        d->hw_type = p->hw_type;
        d->hw_ver = p->hw_ver;
        d->fw_type = p->fw_type;
        d->fw_api_ver = p->fw_api_ver;
        memcpy(d->ver, p->ver, sizeof d->ver);
        d->have_ver = p->have_ver;
    } else {
        d->hw_type = m->hw_type;
        d->fw_type = m->hw_type == 0x03 ? 3 : 2; // SMART_PANEL fwType 3, IO modules 2
    }
}

/**
 * @brief VERSION_QUERY: [fwApiVer verA verB (hwVer)] -> fwVer "fwApiVer.verA.verB".
 *
 * Matches the real CONFIG.JSON. The real CLU skips SMART_PANELs with fwApiVer < 5 (they do not answer).
 */
static void version_query(tfbus_device_t *d, uint8_t addr)
{
    if (d->hw_type == 0x03 && d->fw_api_ver && d->fw_api_ver < 5) {
        return;
    }
    uint8_t f[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];
    size_t n = tfbus_version_query(f, sizeof f, addr);
    int r = tfbus_uart_transact(f, n, rx, sizeof rx, ACK_TIMEOUT_MS);
    const uint8_t *pl;
    size_t plen;
    if (r > 0 && tfbus_parse_response(rx, (size_t)r, &pl, &plen) && plen >= 3) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        d->fw_api_ver = pl[0];
        memcpy(d->ver, pl, 3);
        d->hw_ver = plen >= 4 ? pl[3] : 1;
        d->have_ver = true;
        xSemaphoreGive(s_lock);
    }
}

/**
 * @brief Full bus discovery; rebuilds s_dev.
 *
 * Boot / reload: RESET, SEEK, DISCOVER loop, then ADDR_ASSIGN by SN for the OM.LUA modules that
 * did not answer DISCOVER.
 *
 * Refresh (req_refresh_modules, OM "Clear CLU"): what the real CLU sends (capture
 * test/host/fixtures/capture_refresh_modules_clu2.txt): RESET, ~1.2 s, no SEEK, ADDR_ASSIGN by
 * SN for the OM.LUA modules first, then DISCOVER for whatever is still unaddressed (new modules;
 * known ones that missed their ADDR_ASSIGN), then the rest again, then VERSION_QUERY. Assigning
 * first also means only leftovers answer DISCOVER, instead of every module colliding after RESET.
 *
 * @param refresh true for the req_refresh_modules sequence.
 */
static void discover(bool refresh)
{
    uint8_t f[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];
    size_t n;

    ESP_LOGI(TAG, "%s: %d module(s) in OM.LUA", refresh ? "refresh" : "discovery", s_nmod);
    n = tfbus_reset(f, sizeof f, TFBUS_BROADCAST);
    tfbus_uart_send(f, n);
    vTaskDelay(pdMS_TO_TICKS(refresh ? REFRESH_SETTLE_MS : RESET_SETTLE_MS));

    if (!refresh) {
        n = tfbus_seek(f, sizeof f);
        int r = tfbus_uart_transact(f, n, rx, sizeof rx, ACK_TIMEOUT_MS * 5);
        ESP_LOGI(TAG, "SEEK %s", r > 0 ? "acked" : "no reply");
        (void)r; // log-only
    }

    for (int i = 0; i < s_nmod; i++) {
        tfbus_module_t *m = &s_mod[i];
        m->online = false;
        m->fails = 0;
        m->status_len = 0;
        m->assigned = false;
    }
    // Identity from the previous scan. On refresh the OM.LUA modules are addressed before
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
            assign(&s_mod[i]); // failures retried after DISCOVER
        }
    }
    scan();
    // OM.LUA modules that did not answer DISCOVER (e.g. real Grenton modules): by SN, as before.
    for (int i = 0; i < s_nmod; i++) {
        tfbus_module_t *m = &s_mod[i];
        if (!m->assigned && !assign(m)) {
            ESP_LOGW(TAG, "ADDR_ASSIGN sn=%lu addr=%u: no ACK", (unsigned long)m->sn, m->addr);
        }
    }
    for (int i = 0; i < s_nmod; i++) {
        tfbus_module_t *m = &s_mod[i];
        if (!m->assigned) {
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        tfbus_device_t *d = dev_get(m->sn);
        if (d && !d->hw_type) { // not reported by DISCOVER: last scan's identity, else OM.LUA
            dev_fill(d, m, prev, nprev);
        }
        if (d) {
            d->known = true;
            d->addr = m->addr;
        }
        xSemaphoreGive(s_lock);
    }
    for (int i = 0; i < s_ndev; i++) {
        if (s_dev[i].addr) {
            version_query(&s_dev[i], s_dev[i].addr);
        }
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
static void rejoin(tfbus_module_t *m)
{
    if (!assign(m)) {
        return;
    }
    poll_module(m);
    if (!m->online) {
        return;
    }
    ESP_LOGI(TAG, "addr %u sn=%lu rejoined", m->addr, (unsigned long)m->sn);
    if (m->drv && m->drv->init) {
        m->drv->init(m);
        drain_cmds();
    }
    // Absent at the last discovery (plugged in later): list it in tfbusDevices now, so
    // CONFIG.JSON / config.txt do not wait for the next req_refresh_modules.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tfbus_device_t *d = dev_get(m->sn);
    bool fresh = d && !d->addr;
    if (fresh) {
        if (!d->hw_type) {
            dev_fill(d, m, NULL, 0);
        }
        d->known = true;
        d->addr = m->addr;
    }
    xSemaphoreGive(s_lock);
    if (fresh) {
        version_query(d, m->addr);
        if (s_scan_cb) {
            s_scan_cb();
        }
    }
}



/**
 * @brief STATUS_POLL one module: track online/fails, keep the last status and pass every reply to the driver.
 */
static void poll_module(tfbus_module_t *m)
{
    uint8_t f[TFBUS_MAX_FRAME], rx[TFBUS_MAX_FRAME];
    size_t n = tfbus_status_poll(f, sizeof f, m->addr);
    int r = tfbus_uart_transact(f, n, rx, sizeof rx, POLL_TIMEOUT_MS);
    const uint8_t *pl;
    size_t plen;
    if (r <= 0 || !tfbus_parse_response(rx, (size_t)r, &pl, &plen)) {
        m->fails_total++;
        if (m->fails < 255) {
            m->fails++;
        }
        if (m->fails >= OFFLINE_AFTER) {
            set_online(m, false);
        }
        return;
    }
    m->fails = 0;
    m->polls++;
    set_online(m, true);

    if (plen > sizeof m->status) {
        plen = sizeof m->status;
    }
    bool changed = plen != m->status_len || memcmp(pl, m->status, plen) != 0;
    if (changed) {
        memcpy(m->status, pl, plen);
        m->status_len = (uint8_t)plen;
        if (!m->drv) {
            char hex[3 * TFBUS_MAX_PAYLOAD + 1];
            int o = 0;
            for (size_t i = 0; i < plen; i++) {
                o += snprintf(hex + o, sizeof hex - o, "%02x ", pl[i]);
            }
            ESP_LOGD(TAG, "addr %u status: %s", m->addr, hex);
        }
    }
    // Button events etc. arrive as repeated queued records: drivers see every reply.
    if (m->drv && m->drv->on_status) {
        m->drv->on_status(m, pl, plen);
    }
}

/**
 * @brief Bus task: park until started, discover, init drivers, then poll round-robin until restarted.
 *
 * Queued commands and deferred driver calls are serviced between polls; offline modules get a
 * rejoin attempt every OFFLINE_POLL_EVERY rounds.
 */
static void bus_task(void *arg)
{
    for (;;) {
        // Wait for SYSTEM.Init (modules are known only after OM.LUA ran).
        if (s_park) {
            s_parked = true;
            while (s_park) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
            s_parked = false;
        }
        s_restart = false;
        bool refresh = s_refresh;
        s_refresh = false;
        discover(refresh);
        if (s_scan_cb) {
            s_scan_cb();
        }
        xSemaphoreGive(s_scan_done);
        if (s_nmod == 0) {
            ESP_LOGI(TAG, "no modules in OM.LUA, bus idle");
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        // First round before init, like the real CLU.
        for (int i = 0; i < s_nmod; i++) {
            poll_module(&s_mod[i]);
        }
        for (int i = 0; i < s_nmod; i++) {
            if (s_mod[i].drv && s_mod[i].drv->init) {
                s_mod[i].drv->init(&s_mod[i]);
                drain_cmds(); // init runs in this task: flush before the queue fills up
            }
        }

        uint32_t round = 0;
        int last_online = -1;
        TickType_t last_summary = 0;
        while (!s_restart) {
            for (int i = 0; i < s_nmod && !s_restart; i++) {
                run_deferred(); // driver timer work queues frames: send them right after
                drain_cmds();
                tfbus_module_t *m = &s_mod[i];
                if (!m->online && m->fails >= OFFLINE_AFTER) {
                    if (round % OFFLINE_POLL_EVERY == 0) {
                        rejoin(m);
                    }
                    continue;
                }
                poll_module(m);
            }
            round++;
            if (round % 500 == 0) { // summary only on change, else every SUMMARY_EVERY_MS
                int online = 0;
                for (int i = 0; i < s_nmod; i++) {
                    online += s_mod[i].online;
                }
                TickType_t t = xTaskGetTickCount();
                if (online != last_online || t - last_summary >= pdMS_TO_TICKS(SUMMARY_EVERY_MS)) {
                    ESP_LOGI(TAG, "round %lu: %d/%d online", (unsigned long)round, online, s_nmod);
                    last_online = online;
                    last_summary = t;
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
    clu_lua_register_class(2, &s_module_ops);
    // Any class may be hosted on a module; virtual-object implementations registered later
    // (e.g. Timer) override these. 0/1 = CLUs, 44 = storage.
    for (int c = 3; c < 128; c++) {
        if (c != 44) {
            clu_lua_register_class(c, &s_io_ops);
        }
    }
    xTaskCreatePinnedToCore(bus_task, "tfbus", 6144, NULL, 10, &s_task, 1);
}

void tfbus_master_start(void)
{
    s_park = false;
    s_restart = true;
    xTaskNotifyGive(s_task);
}

bool tfbus_master_reset(uint32_t timeout_ms)
{
    s_park = true;
    s_restart = true;
    xTaskNotifyGive(s_task);
    // Discovery and the first round do not check s_restart: allow for a full one.
    for (uint32_t t = 0; !s_parked; t += 10) {
        if (t >= timeout_ms) {
            ESP_LOGE(TAG, "reset: bus task did not stop");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    for (int i = 0; i < s_nmod; i++) {
        tfbus_module_t *m = &s_mod[i];
        if (m->drv_ctx && m->drv && m->drv->deinit) {
            m->drv->deinit(m);
        }
    }
    // A driver esp_timer callback that fired just before deinit may still be running and using
    // its context: let it finish before the context goes away.
    vTaskDelay(pdMS_TO_TICKS(20));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_nmod; i++) {
        free(s_mod[i].drv_ctx);
        memset(&s_mod[i], 0, sizeof s_mod[i]);
    }
    s_nmod = 0;
    xSemaphoreGive(s_lock);
    xQueueReset(s_cmds); // frames queued for the old module set
    xQueueReset(s_defer); // deferred calls point at the freed driver contexts
    ESP_LOGI(TAG, "reset: module registry cleared");
    return true;
}

void tfbus_master_on_scan(void (*cb)(void))
{
    s_scan_cb = cb;
}

bool tfbus_master_refresh(uint32_t timeout_ms)
{
    if (!s_task) {
        return false;
    }
    xSemaphoreTake(s_scan_done, 0); // drop a stale completion
    s_refresh = true;
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
    int n = s_ndev < max ? s_ndev : max;
    memcpy(out, s_dev, (size_t)n * sizeof *out);
    xSemaphoreGive(s_lock);
    return n;
}
