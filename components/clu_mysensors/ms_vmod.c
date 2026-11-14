/**
 * @file ms_vmod.c
 * @brief MySensors sensors as emulated TF-Bus modules (see ms_vmod.h).
 */
#include "ms_vmod.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "tfbus_frame.h"
#include "tfbus_uart.h"
#include "tfbus_vmod.h"

static const char *TAG = "mysensors";

#define NVS_NS "mysensors"
#define NVS_SN "vsn"
#define NVS_MAP "vmap"
#define NVS_OWN "vown"

/** @brief MySensors types used here. */
enum {
    S_DOOR = 0, S_MOTION = 1, S_SMOKE = 2, S_BINARY = 3, S_LOCK = 19, S_SPRINKLER = 31,
    S_WATER_LEAK = 32, S_SOUND = 33, S_VIBRATION = 34, S_MOISTURE = 35,
    V_STATUS = 2, V_TRIPPED = 16, V_LOCK_STATUS = 36,
};

/** @brief Which slot kind (and value type) a presented S_ type gets. */
static const struct {
    uint8_t stype;
    uint8_t kind;
    uint8_t vtype;
} RULES[] = {
    {S_DOOR, MS_SLOT_DIN, V_TRIPPED},       {S_MOTION, MS_SLOT_DIN, V_TRIPPED},
    {S_SMOKE, MS_SLOT_DIN, V_TRIPPED},      {S_WATER_LEAK, MS_SLOT_DIN, V_TRIPPED},
    {S_SOUND, MS_SLOT_DIN, V_TRIPPED},      {S_VIBRATION, MS_SLOT_DIN, V_TRIPPED},
    {S_MOISTURE, MS_SLOT_DIN, V_TRIPPED},   {S_BINARY, MS_SLOT_DOUT, V_STATUS},
    {S_SPRINKLER, MS_SLOT_DOUT, V_STATUS},  {S_LOCK, MS_SLOT_DOUT, V_LOCK_STATUS},
};

/** @brief Binding of one slot (NVS layout: 4 bytes). */
typedef struct {
    uint8_t used, node, child, vtype;
} slot_t;

/** @brief Emulated module type: bus behaviour and slot layout. */
typedef struct {
    const tfbus_vmod_ops_t *ops;
    uint8_t first[MS_SLOT_KINDS];  /**< First slot of each kind. */
    uint8_t count[MS_SLOT_KINDS];  /**< Slots of each kind (0 = kind not on this type). */
} vtype_t;

/** @brief One emulated module. */
typedef struct {
    const vtype_t *type;
    uint8_t in, out;               /**< DIN8: input / output bits. */
} vmod_t;

static ms_vmod_cfg_t s_cfg;
static SemaphoreHandle_t s_mx;
static tfbus_vmod_t s_bus[MS_VMOD_MAX];   // bus side; ctx -> s_mod[i]
static vmod_t s_mod[MS_VMOD_MAX];
static slot_t s_slot[MS_VMOD_MAX][MS_VMOD_SLOTS];
static uint32_t s_sn[MS_VMOD_MAX];        // NVS image of the serials (0 = not created)
static uint8_t s_owner[MS_VMOD_MAX];      // node the module belongs to (0 = none: free / mixed)
static int s_n;                           // modules created; read by the bus task without the lock
static int s_max;

// ---------------------------------------------------------------------------------------------
// IO_MODULE_DIN_8
// ---------------------------------------------------------------------------------------------
static int din8_status(tfbus_vmod_t *bus_endpoint, uint8_t *status_buffer, size_t cap)
{
    const vmod_t *virtual_module = bus_endpoint->ctx;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    tfbus_din8_status(virtual_module->in, virtual_module->out, status_buffer);
    xSemaphoreGive(s_mx);
    return TFBUS_DIN8_STATUS_LEN;
}

/** @brief MODULE_CMD: switch outputs; each bound output that changed is sent to its node. */
static bool din8_command(tfbus_vmod_t *bus_endpoint, uint8_t type, const uint8_t *command_payload, size_t plen)
{
    if (type != TFBUS_T_MODULE_CMD) {
        return true;
    }
    vmod_t *virtual_module = bus_endpoint->ctx;
    int idx = (int)(virtual_module - s_mod);
    slot_t send[8];
    uint8_t val[8];
    int changed_count = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    uint8_t out = virtual_module->out;
    bool ok = tfbus_din8_cmd(command_payload, plen, &out);
    if (ok) {
        uint8_t changed = out ^ virtual_module->out;
        for (int ch = 0; ch < 8; ch++) {
            const slot_t *slot_entry = &s_slot[idx][virtual_module->type->first[MS_SLOT_DOUT] + ch];
            if ((changed >> ch & 1) && slot_entry->used) {
                send[changed_count] = *slot_entry;
                val[changed_count++] = out >> ch & 1;
            }
        }
        virtual_module->out = out;
    }
    xSemaphoreGive(s_mx);
    for (int i = 0; i < changed_count; i++) {
        if (!s_cfg.send(send[i].node, send[i].child, 1 /* C_SET */, send[i].vtype, val[i] ? "1" : "0")) {
            ESP_LOGW(TAG, "node %u/%u: output not sent (queue full)", send[i].node, send[i].child);
        }
    }
    return ok;
}

static const tfbus_vmod_ops_t DIN8_OPS = {
    .hw_type = TFBUS_DIN8_HW_TYPE,
    .fw_type = TFBUS_DIN8_FW_TYPE,
    .fw_api_ver = TFBUS_DIN8_FW_API_VER,
    .version = {TFBUS_DIN8_FW_API_VER, 2, 4, 3},
    .status = din8_status,
    .command = din8_command,
};

static const vtype_t DIN8 = {
    .ops = &DIN8_OPS,
    .first = {[MS_SLOT_DIN] = 0, [MS_SLOT_DOUT] = 8},
    .count = {[MS_SLOT_DIN] = 8, [MS_SLOT_DOUT] = 8},
};

// ---------------------------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------------------------
static void save_map(void)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: bindings not saved");
        return;
    }
    nvs_set_blob(nvs_handle, NVS_MAP, s_slot, sizeof s_slot);
    nvs_set_blob(nvs_handle, NVS_OWN, s_owner, sizeof s_owner);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

static void save_sn(void)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: module serials not saved");
        return;
    }
    nvs_set_blob(nvs_handle, NVS_SN, s_sn, sizeof s_sn);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

/** @brief Set up module @p i (serial in s_sn[i]) as an IO_MODULE_DIN_8; not visible until s_n covers it. */
static void setup_module(int module_index)
{
    s_mod[module_index] = (vmod_t){.type = &DIN8};
    s_bus[module_index] = (tfbus_vmod_t){.sn = s_sn[module_index], .ops = DIN8.ops, .ctx = &s_mod[module_index]};
}

/** @brief Create one more module (fresh serial, saved); -1 at the limit. Lock held. */
static int add_module(void)
{
    if (s_n >= s_max) {
        return -1;
    }
    int module_index = s_n;
    uint32_t serial_number;
    bool dup;
    do { // non-zero, 31 bits like the CLU and on-board IO serials, distinct
        serial_number = esp_random() & 0x7fffffff;
        dup = serial_number == 0;
        for (int j = 0; j < module_index; j++) {
            dup |= s_sn[j] == serial_number;
        }
    } while (dup);
    s_sn[module_index] = serial_number;
    memset(s_slot[module_index], 0, sizeof s_slot[module_index]);
    setup_module(module_index);
    save_sn();
    __atomic_store_n(&s_n, module_index + 1, __ATOMIC_RELEASE); // the bus endpoint sees it fully set up
    ESP_LOGW(TAG, "new module %d: IO_MODULE_DIN_8 SN %lu; run discovery in OM to add it", module_index + 1, (unsigned long)serial_number);
    return module_index;
}

static bool module_empty(int module_index)
{
    for (int j = 0; j < MS_VMOD_SLOTS; j++) {
        if (s_slot[module_index][j].used) {
            return false;
        }
    }
    return true;
}

/** @brief Modules of @p node, in creation order; returns how many. Lock held. */
static int node_modules(uint8_t node, int *out)
{
    int module_count = 0;
    for (int i = 0; i < s_n; i++) {
        if (s_owner[i] == node) {
            out[module_count++] = i;
        }
    }
    return module_count;
}

/** @brief One more module for @p node: an empty unowned one, else a new one; -1 at the limit. Lock held. */
static int take_module(uint8_t node)
{
    int module_index = 0;
    while (module_index < s_n && (s_owner[module_index] || !module_empty(module_index))) {
        module_index++;
    }
    if (module_index == s_n && (module_index = add_module()) < 0) {
        return -1;
    }
    s_owner[module_index] = node;
    s_mod[module_index].in = s_mod[module_index].out = 0;
    return module_index;
}

/** @brief Slot kind of channel @p slot of module @p i, and its channel within the kind. */
static int kind_of(int module_index, int slot, int *channel_index)
{
    const vtype_t *module_type = s_mod[module_index].type;
    for (int k = 0; k < MS_SLOT_KINDS; k++) {
        if (module_type->count[k] && slot >= module_type->first[k] && slot < module_type->first[k] + module_type->count[k]) {
            *channel_index = slot - module_type->first[k];
            return k;
        }
    }
    return -1;
}

/** @brief A node's sensor of one kind with its state bit (for the re-sort). */
typedef struct {
    uint8_t child, vtype, bit;
} entry_t;

static int entry_cmp(const void *left_entry_ptr, const void *right_entry_ptr)
{
    const entry_t *left_entry = left_entry_ptr, *right_entry = right_entry_ptr;
    return left_entry->child != right_entry->child ? left_entry->child - right_entry->child : left_entry->vtype - right_entry->vtype;
}

/**
 * @brief Lay out a node's sensors of one kind over its own modules in child-id order, with @p add
 * (a newly presented sensor, NULL = none) among them: a lower id presented late moves the others
 * up one, their state bits with them; a sensor taken away (kind changed) closes its gap. Modules the
 * node no longer needs are released for the next node. Lock held.
 */
static void layout(uint8_t node, uint8_t kind, const entry_t *add)
{
    const int per = DIN8.count[kind], first = DIN8.first[kind];
    const char *kname = kind == MS_SLOT_DIN ? "DIN" : "DOUT";
    int mods[MS_VMOD_MAX], module_count = node_modules(node, mods);
    entry_t entries[MS_VMOD_MAX * 8 + 1];
    int entry_count = 0;
    for (int k = 0; k < module_count; k++) {
        const vmod_t *virtual_module = &s_mod[mods[k]];
        uint8_t bits = kind == MS_SLOT_DIN ? virtual_module->in : virtual_module->out;
        for (int c = 0; c < per; c++) {
            const slot_t *slot_entry = &s_slot[mods[k]][first + c];
            if (slot_entry->used) {
                entries[entry_count++] = (entry_t){.child = slot_entry->child, .vtype = slot_entry->vtype, .bit = (uint8_t)(bits >> c & 1)};
            }
        }
    }
    if (add) {
        entries[entry_count++] = *add;
    }
    qsort(entries, (size_t)entry_count, sizeof *entries, entry_cmp);
    while (module_count * per < entry_count) {
        int i = take_module(node);
        if (i < 0) {
            ESP_LOGW(TAG, "node %u/%u: no free %s channel, %d module(s) = the limit (menuconfig "
                          "CLU_MYSENSORS_MODULES_MAX)", node, add ? add->child : 255, kname, s_max);
            return;
        }
        mods[module_count++] = i;
    }
    bool moved = false;
    for (int i = 0; i < module_count * per; i++) {
        int module_index = mods[i / per], c = i % per;
        slot_t *slot_entry = &s_slot[module_index][first + c];
        uint8_t *bits = kind == MS_SLOT_DIN ? &s_mod[module_index].in : &s_mod[module_index].out;
        if (i >= entry_count) { // past the node's last sensor of this kind
            moved |= slot_entry->used;
            *slot_entry = (slot_t){0};
            *bits &= (uint8_t)~(1u << c);
            continue;
        }
        if (add && entries[i].child == add->child && entries[i].vtype == add->vtype) {
            ESP_LOGI(TAG, "node %u/%u -> module %d (SN %lu) %s %d", node, add->child, module_index + 1,
                     (unsigned long)s_sn[module_index], kname, c + 1);
        } else if (!slot_entry->used || slot_entry->child != entries[i].child || slot_entry->vtype != entries[i].vtype) {
            moved = true;
        }
        *slot_entry = (slot_t){.used = 1, .node = node, .child = entries[i].child, .vtype = entries[i].vtype};
        *bits = entries[i].bit ? (uint8_t)(*bits | 1u << c) : (uint8_t)(*bits & ~(1u << c));
    }
    if (moved) {
        ESP_LOGW(TAG, "node %u: %s channels re-sorted by child id", node, kname);
    }
    for (int k = 0; k < module_count; k++) {
        if (module_empty(mods[k])) {
            s_owner[mods[k]] = 0; // free for the next node that needs one
        }
    }
    save_map();
}

void ms_vmod_present(uint8_t node, uint8_t child, uint8_t stype, const char *desc)
{
    if (!s_max || node == 0 || node == 255) {
        return; // the gateway / a node without an id
    }
    int rule_index = 0;
    while (rule_index < (int)(sizeof RULES / sizeof *RULES) && RULES[rule_index].stype != stype) {
        rule_index++;
    }
    if (rule_index == (int)(sizeof RULES / sizeof *RULES)) {
        return;
    }
    uint8_t kind = RULES[rule_index].kind;
    const uint8_t vtype = RULES[rule_index].vtype;
    // WORKAROUND: some relay/input boards present their inputs as S_BINARY too (V_STATUS, like the
    // relays), so the type cannot tell them apart; such a child whose description starts with
    // "INPUT" is taken as a binary input (DIN fed by V_STATUS) instead of an output.
    if (stype == S_BINARY && desc && strncasecmp(desc, "INPUT", 5) == 0) {
        kind = MS_SLOT_DIN;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < s_n; i++) {
        for (int j = 0; j < MS_VMOD_SLOTS; j++) {
            slot_t *slot_entry = &s_slot[i][j];
            int channel_index;
            if (slot_entry->used && slot_entry->node == node && slot_entry->child == child && slot_entry->vtype == vtype) {
                int had = kind_of(i, j, &channel_index);
                if (had == kind) {
                    xSemaphoreGive(s_mx); // bound already
                    return;
                }
                *slot_entry = (slot_t){0}; // bound as the other kind: move it
                layout(node, (uint8_t)had, NULL);
                i = s_n; // done searching
                break;
            }
        }
    }
    layout(node, kind, &(entry_t){.child = child, .vtype = vtype});
    xSemaphoreGive(s_mx);
}

void ms_vmod_value(uint8_t node, uint8_t child, uint8_t vtype, const char *payload)
{
    if (!s_n) {
        return;
    }
    bool on = strtod(payload, NULL) != 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < s_n; i++) {
        for (int j = 0; j < MS_VMOD_SLOTS; j++) {
            const slot_t *slot_entry = &s_slot[i][j];
            int channel_index;
            int slot_kind = slot_entry->used && slot_entry->node == node && slot_entry->child == child && slot_entry->vtype == vtype ? kind_of(i, j, &channel_index) : -1;
            uint8_t *bits = slot_kind == MS_SLOT_DIN ? &s_mod[i].in : slot_kind == MS_SLOT_DOUT ? &s_mod[i].out : NULL;
            if (bits) {
                *bits = on ? (uint8_t)(*bits | 1u << channel_index) : (uint8_t)(*bits & ~(1u << channel_index));
            }
        }
    }
    xSemaphoreGive(s_mx);
}

int ms_vmod_frame(const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxcap)
{
    return tfbus_vmod_frame(s_bus, __atomic_load_n(&s_n, __ATOMIC_ACQUIRE), tx, txlen, rx, rxcap);
}

int ms_vmod_list(ms_vmod_slot_t *out, int max)
{
    int slot_count = 0;
    if (!s_n) {
        return 0;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < s_n; i++) {
        for (int j = 0; j < MS_VMOD_SLOTS && slot_count < max; j++) {
            const slot_t *slot_entry = &s_slot[i][j];
            int channel_index, slot_kind = slot_entry->used ? kind_of(i, j, &channel_index) : -1;
            if (slot_kind >= 0) {
                out[slot_count++] = (ms_vmod_slot_t){.module = i, .sn = s_bus[i].sn, .hw_type = s_bus[i].ops->hw_type,
                                            .kind = (ms_slot_kind_t)slot_kind, .channel = channel_index, .node = slot_entry->node,
                                            .child = slot_entry->child, .vtype = slot_entry->vtype};
            }
        }
    }
    xSemaphoreGive(s_mx);
    return slot_count;
}

int ms_vmod_owner(int module)
{
    return module >= 0 && module < s_n ? s_owner[module] : -1;
}

int ms_vmod_unmap(uint8_t node, uint8_t child)
{
    int unmapped_count = 0;
    if (!s_n) {
        return 0;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < MS_VMOD_MAX; i++) {
        for (int j = 0; j < MS_VMOD_SLOTS; j++) {
            slot_t *slot_entry = &s_slot[i][j];
            if (slot_entry->used && slot_entry->node == node && (child == 255 || slot_entry->child == child)) {
                memset(slot_entry, 0, sizeof *slot_entry);
                unmapped_count++;
            }
        }
    }
    for (int i = 0; i < s_n; i++) {
        if (s_owner[i] == node && module_empty(i)) {
            s_owner[i] = 0; // free for the next node that needs one
        }
    }
    if (unmapped_count) {
        save_map();
    }
    xSemaphoreGive(s_mx);
    return unmapped_count;
}

int ms_vmod_prune(const uint8_t alive[32])
{
    if (!s_n) {
        return 0;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int dropped = 0;
    uint8_t logged[32] = {0};
    for (int i = 0; i < s_n; i++) {
        for (int j = 0; j < MS_VMOD_SLOTS; j++) {
            slot_t *slot_entry = &s_slot[i][j];
            const uint8_t bit = (uint8_t)(1u << (slot_entry->node % 8));
            if (slot_entry->used && !(alive[slot_entry->node / 8] & bit)) {
                if (!(logged[slot_entry->node / 8] & bit)) {
                    logged[slot_entry->node / 8] |= bit;
                    ESP_LOGW(TAG, "node %u not heard: its channels removed", slot_entry->node);
                }
                *slot_entry = (slot_t){0};
                dropped++;
            }
        }
    }
    // empty modules go: the ones after them move down (the RESET that follows re-addresses all)
    int compacted_count = 0, removed = 0;
    for (int i = 0; i < s_n; i++) {
        if (module_empty(i)) {
            ESP_LOGW(TAG, "module SN %lu removed (no channels)", (unsigned long)s_sn[i]);
            removed++;
            continue;
        }
        if (compacted_count != i) {
            s_sn[compacted_count] = s_sn[i];
            s_owner[compacted_count] = s_owner[i];
            memcpy(s_slot[compacted_count], s_slot[i], sizeof s_slot[compacted_count]);
            vmod_t module_snapshot = s_mod[i];
            setup_module(compacted_count);
            s_mod[compacted_count].in = module_snapshot.in;
            s_mod[compacted_count].out = module_snapshot.out;
        }
        compacted_count++;
    }
    for (int i = compacted_count; i < s_n; i++) {
        s_sn[i] = 0;
        s_owner[i] = 0;
        memset(s_slot[i], 0, sizeof s_slot[i]);
    }
    __atomic_store_n(&s_n, compacted_count, __ATOMIC_RELEASE);
    if (dropped || removed) {
        save_sn();
        save_map();
    }
    xSemaphoreGive(s_mx);
    return removed;
}

/** @brief Read a blob of up to @p cap bytes (shorter ones from an older, smaller layout too). */
static void load_blob(nvs_handle_t storage_handle, const char *key, void *out, size_t cap, size_t unit)
{
    size_t len = 0;
    memset(out, 0, cap);
    if (nvs_get_blob(storage_handle, key, NULL, &len) != ESP_OK || len == 0 || len > cap || len % unit) {
        return;
    }
    if (nvs_get_blob(storage_handle, key, out, &len) != ESP_OK) {
        memset(out, 0, cap);
    }
}

int ms_vmod_init(const ms_vmod_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_max = cfg->max < 0 ? 0 : cfg->max > MS_VMOD_MAX ? MS_VMOD_MAX : cfg->max;
    s_n = 0;
    if (!s_max) {
        return 0;
    }
    if (!s_mx) {
        s_mx = xSemaphoreCreateMutex();
    }
    nvs_handle_t storage_handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &storage_handle) == ESP_OK) {
        load_blob(storage_handle, NVS_SN, s_sn, sizeof s_sn, sizeof s_sn[0]);
        load_blob(storage_handle, NVS_MAP, s_slot, sizeof s_slot, sizeof s_slot[0]);
        load_blob(storage_handle, NVS_OWN, s_owner, sizeof s_owner, 1);
        nvs_close(storage_handle);
    } else {
        memset(s_sn, 0, sizeof s_sn);
        memset(s_slot, 0, sizeof s_slot);
        memset(s_owner, 0, sizeof s_owner);
    }
    int module_count = 0;
    while (module_count < s_max && s_sn[module_count]) {
        setup_module(module_count);
        ESP_LOGI(TAG, "module %d: IO_MODULE_DIN_8 SN %lu", module_count + 1, (unsigned long)s_sn[module_count]);
        module_count++;
    }
    s_n = module_count;
    for (int i = 0; i < s_n; i++) {
        if (s_owner[i]) {
            continue;
        }
        // saved before modules had an owner: a module holding one node's sensors only is that node's
        uint8_t node = 0;
        bool mixed = false;
        for (int j = 0; j < MS_VMOD_SLOTS; j++) {
            if (s_slot[i][j].used) {
                mixed |= node && node != s_slot[i][j].node;
                node = s_slot[i][j].node;
            }
        }
        s_owner[i] = mixed ? 0 : node;
    }
    tfbus_uart_add_local(ms_vmod_frame);
    return s_n;
}
