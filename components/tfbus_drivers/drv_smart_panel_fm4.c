/**
 * @file drv_smart_panel_fm4.c
 * @brief SMART_PANEL_FM_4 driver (hwType 3 / 0x03, fwType 3).
 *
 * Objects: 16x PANEL_BUTTON (class 30), PANEL (29), PANELSENSTEMP (20), PANELSENSLIGHT (21),
 * 4x PANEL_PAGE (19). Doc: docs/TFBUS_SMART_PANEL_FM_4.md.
 *
 * Init: the real CLU sends no panel-specific init of its own. Every frame is caused by an OM.LUA
 * set() on the panel's objects, emitted in OM.LUA line order (pass 1). After OM.LUA it re-sends
 * everything in reverse order (pass 2). Replicated frame-for-frame from captures of a real CLU
 * (clu firmware/tfbus_capture_20261014_171505.log addr 0x12 and the 2026-11-09 cold boot). Both
 * had empty labels, icons and pages.
 * @verbatim
 *   pass 1:
 *     PANEL_BUTTON b=0..15: class 0x17 [mask16 LE, 0]; DISPLAY_BLOCK 0x30 [b, 0x0a|0x00|0x01, 20x 0]
 *     PANEL set(4..7) gesture icons: 0x30 [0, sub, name] sub up=04 down=05 left=02 right=03
 *     PANEL set(10..21): TFBUS_CMD 0x31 [reg, val LE] (reg table k_regs)
 *     sensors: class 0x16 [sensor, Sensitivity / 100 ms (max 255), Threshold x 10 LE16], sensor 00
 *       temperature, 01 light; one per set() of Threshold / Sensitivity (MinValue / MaxValue send
 *       nothing), Sensitivity 01 until it is set; checked on a real CLU. The panel applies both.
 *     PANEL_PAGE p=1..4 set(0,1,3..10): 0x31 [12, p, field, slot] field 00/01 header, 03/04 per slot
 *   pass 2 (reverse): pages 4..1 incl. 0x31 [1a, button, 02, 00] + [12, p, 06, slot];
 *     sensors 2 of 4; regs with PageNr=0; icons; buttons 15..0 with LED_CMD 0x18 [00,00,02]
 *     right before button 0's blocks.
 * @endverbatim
 * PANEL_PAGE texts (runtime set() on a real CLU): 0x31 [12, p, field, slot, text...] (text up to 18
 * bytes, zero padded): PageName field 01 slot 00, Object_1_Name field 04 slot 01. Object_n_Id /
 * Object_n_Name of slots 2..4 (field 03 / 04, slot n) follow the init headers (inferred).
 * NOT YET DECODED: button labels / icons, PageType values, CustomIcon, DistributedLogicGroup.
 *
 * Runtime: LED_CMD 0x18 [mask, 0, cmd]; TFBUS_CMD (PANEL execute -> sub-cmd): SwitchOnDisplay 0x00,
 * ShowButtons 0x03 [0,0,0,0], ClearScreen 0x04, PrintText 0x0a, DisplayContent 0x09, Draw 0x19,
 * SetBeep 0x0f; PANEL set(4..7, 10..21) re-sends its frame.
 *
 * Status: the real SMART_PANEL_FM_4 answers a poll with a bare ACK, or with the 13-byte state
 * frame the real CLU reads by position (firmware.bin 0xb14a2 PANEL_BUTTON, 0xb1526 sensors):
 * `changed(LE16) | values(LE16) | temp(LE16, 0.1 C) | light(LE16, 0.1 %) | 00 00 | ext 00 | s2(2)`.
 * light checked against PANELSENSLIGHT Value on a real CLU (19 -> 1.9 %, 583 -> 58.3 %); s2 does
 * not follow the light (probably the raw proximity reading).
 * A button's Value is taken from `values` only while its bit in `changed` is set; the CLU derives
 * the button events from Value itself (the panel sends none). Bistable / Locked are done by the
 * panel from the Mode in class frame 0x17 [mask16, mode]. TLV records 02 temp / 07 light raw are
 * still accepted.
 *
 * ext (byte 10), checked against PANEL events on a real CLU, each flag in one frame only:
 * @verbatim
 *   01 OnProximityDetect   02 OnGestureUp   04 OnGestureDown   08 OnGestureLeft   10 OnGestureRight
 *   80 display record, all other bytes 0 except byte 0: 01 OnDisplayOn, 00 OnDisplayOff
 * @endverbatim
 * A wake sends the display-on record, then a frame with 01. OnPageChange (5) was not seen
 * (PageControlMode 1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "drv_panel_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include "tfbus_drivers.h"
#include "tfbus_master.h"

static const char *TAG = "drv_sp4";

#define BTN 16
/** @brief OM object classes hosted on the panel. */
enum { CLS_PAGE = 19, CLS_TEMP = 20, CLS_LIGHT = 21, CLS_PANEL = 29, CLS_BUTTON = 30 };
#define SENSOR_FRAME_LEN 13 /**< Status payload length of the non-TLV sensor frame. */
/** @brief Sensor ids of the class frame 0x16. */
enum { SENSOR_TEMP = 0, SENSOR_LIGHT = 1 };
#define SENSITIVITY_UNSET_MS 100 /**< Sent before OM.LUA sets Sensitivity (wire 01). */
#define EXT_BYTE 10         /**< Position of ext in the sensor frame. */
#define EXT_DISPLAY 0x80    /**< ext: display record (byte 0 = display on). */
/** @brief LED_CMD command codes. */
enum { LED_OK = 0x00, LED_ERROR = 0x01, LED_ON = 0x02, LED_OFF = 0x04, LED_RED_ON = 0x10 };

#define PAGES 4
#define SLOTS 4
#define ICON_LEN 20
#define PAGE_TEXT_LEN 18 /**< 0x31 payload 22 = cmd 0x12, page, field, slot + text. */
/** @brief PANEL_PAGE header fields (0x31 0x12 [p, field, slot]). */
enum { PAGE_FIELD_TYPE = 0x00, PAGE_FIELD_NAME = 0x01, PAGE_FIELD_OBJECT_ID = 0x03, PAGE_FIELD_OBJECT_NAME = 0x04,
       PAGE_FIELD_CUSTOM_ICON = 0x06 };

/**
 * @brief PANEL feature -> TFBUS_CMD register, with the PANEL defaults (doc: TFBUS_SMART_PANEL_FM_4.md).
 */
static const struct {
    uint8_t feature, reg;
    uint16_t def;
} k_regs[] = {
    {10, 0x01, 5},    // ProximitySens
    {11, 0x02, 5000}, // ProximityTimeout ms
    {13, 0x0c, 1},    // BuzzerValue
    {15, 0x0d, 3},    // GestureMode
    {16, 0x0e, 2},    // GestureSens
    {17, 0x10, 1},    // PageNr
    {18, 0x11, 0},    // PageDisplayMode
    {19, 0x14, 1},    // ButtonsLEDMode
    {20, 0x15, 1},    // PageControlMode
    {21, 0x16, 1},    // GestureDisplayMode
};
#define NREGS (sizeof k_regs / sizeof k_regs[0])
/** @brief Index of PageNr in k_regs. */
enum { REG_PAGENR = 5 }; // index of PageNr in k_regs

/** @brief PANEL features 4..7 (GestureIconUp/Down/Left/Right) -> DISPLAY_BLOCK sub-type. */
static const uint8_t k_icon_sub[4] = {0x04, 0x05, 0x02, 0x03};

/** @brief PANEL_BUTTON feature 1 Mode, sent to the panel in class frame 0x17 (the panel applies it). */
enum { MODE_MONOSTABLE = 0, MODE_BISTABLE = 1, MODE_LOCKED = 2 };
/** @brief PANEL_BUTTON events. */
enum { EV_CHANGE = 0, EV_ON = 1, EV_OFF = 2, EV_SHORT = 3, EV_LONG = 4, EV_HOLD = 5, EV_CLICK = 6 };
/** @brief PANEL events. */
enum { PEV_GESTURE_UP = 0, PEV_GESTURE_DOWN = 1, PEV_GESTURE_LEFT = 2, PEV_GESTURE_RIGHT = 3,
       PEV_PROXIMITY = 4, PEV_PAGE_CHANGE = 5, PEV_DISPLAY_ON = 6, PEV_DISPLAY_OFF = 7 };

/** @brief ext flag -> PANEL event, in the order they are posted when several are set. */
static const struct {
    uint8_t flag, event;
} k_ext_events[] = {
    {0x01, PEV_PROXIMITY}, {0x02, PEV_GESTURE_UP}, {0x04, PEV_GESTURE_DOWN},
    {0x08, PEV_GESTURE_LEFT}, {0x10, PEV_GESTURE_RIGHT},
};

/** @brief Driver state per module (drv_ctx). */
typedef struct {
    char btn[BTN][24];
    char panel_name[24];           // PANEL object (empty = not declared)
    uint16_t btn_value;            // Value bits as last reported by the panel
    uint8_t mode[BTN];             // PANEL_BUTTON Mode (feature 1)
    uint16_t hold_delay[BTN];      // ms (feature 2)
    uint16_t hold_interval[BTN];   // ms (feature 3)
    int64_t pressed_at[BTN];       // us, 0 = not held (monostable only)
    int64_t next_hold[BTN];        // us
    panel_sensor_t temp, light;
    uint16_t reg[NREGS];
    char icon[4][ICON_LEN + 1];
    struct {
        char name[PAGE_TEXT_LEN + 1];
        char object_id[SLOTS][PAGE_TEXT_LEN + 1];
        char object_name[SLOTS][PAGE_TEXT_LEN + 1];
    } page[PAGES];                 // PANEL_PAGE texts, sent with their headers
    bool initialized;
    bool page_warned;
} sp4_t;

static void led(tfbus_module_t *module, int button_index, uint8_t cmd);
static void send_mode(tfbus_module_t *module, int button_index);

/** @brief Driver state of @p module, allocated (with sensor / register defaults) on first use. */
static sp4_t *ctx(tfbus_module_t *module)
{
    if (!module->drv_ctx) {
        sp4_t *panel_state = calloc(1, sizeof *panel_state);
        if (!panel_state) {
            abort(); // out of memory while loading OM.LUA: reboot instead of a NULL dereference
        }
        panel_state->temp = (panel_sensor_t){.threshold = 10, .min = 0, .max = 4500, .sensitivity_ms = 1000};
        panel_state->light = (panel_sensor_t){.threshold = 50, .min = 0, .max = 10000, .sensitivity_ms = 500};
        for (size_t i = 0; i < NREGS; i++) {
            panel_state->reg[i] = k_regs[i].def;
        }
        for (int b = 0; b < BTN; b++) {
            panel_state->hold_delay[b] = 1000;
            panel_state->hold_interval[b] = 100;
        }
        module->drv_ctx = panel_state;
    }
    return module->drv_ctx;
}

/** @brief Queue a TFBUS_CMD (0x31) frame with sub-command @p cmd. */
static void tfbus_cmd_queue(tfbus_module_t *module, uint8_t cmd, const uint8_t *data, size_t len)
{
    uint8_t frame[TFBUS_MAX_FRAME];
    size_t frame_length = tfbus_cmd(frame, sizeof frame, module->addr, cmd, data, len);
    if (frame_length) {
        tfbus_master_queue(frame, frame_length);
    }
}

/** @brief Send PANEL register k_regs[@p i] = @p v. */
static void send_reg(tfbus_module_t *module, size_t register_index, uint16_t register_value)
{
    tfbus_cmd_queue(module, k_regs[register_index].reg, (uint8_t[]){(uint8_t)register_value, (uint8_t)(register_value >> 8)}, 2);
}

/** @brief Send gesture icon @p i as a DISPLAY_BLOCK (0x30). */
static void send_icon(tfbus_module_t *module, int icon_index)
{
    uint8_t blk[22] = {0, k_icon_sub[icon_index]};
    memcpy(blk + 2, ctx(module)->icon[icon_index], strlen(ctx(module)->icon[icon_index]));
    drv_queue_frame(module, 0x30, blk, sizeof blk);
}

/**
 * @brief Send one button's class frame (0x17) and DISPLAY_BLOCKs; optionally the LED commit (pass 2).
 */
static void send_button(tfbus_module_t *module, int button_index, bool led_commit)
{
    send_mode(module, button_index);
    if (led_commit) {
        drv_queue_frame(module, 0x18, (uint8_t[]){0x00, 0x00, 0x02}, 3);
    }
    static const uint8_t subs[3] = {0x0a, 0x00, 0x01};
    for (int i = 0; i < 3; i++) {
        uint8_t blk[22] = {(uint8_t)button_index, subs[i]};
        drv_queue_frame(module, 0x30, blk, sizeof blk);
    }
}

/** @brief Send one button's Mode: class frame 0x17 [mask16 LE, mode] (byte 3 = Mode, seen on a real CLU). */
static void send_mode(tfbus_module_t *module, int button_index)
{
    uint16_t mask = (uint16_t)(1u << button_index);
    drv_queue_frame(module, 0x17, (uint8_t[]){(uint8_t)mask, (uint8_t)(mask >> 8), ctx(module)->mode[button_index]}, 3);
}

static char *page_text(sp4_t *panel_state, int page_number, uint8_t field, uint8_t slot);
static void send_sensor_config(tfbus_module_t *module, int sensor_id, uint16_t sensitivity_ms);

/** @brief Send a PANEL_PAGE field: TFBUS_CMD 0x12 [p, field, slot, stored text]. */
static void send_page(tfbus_module_t *module, uint8_t page_number, uint8_t field, uint8_t slot)
{
    uint8_t page_data[3 + PAGE_TEXT_LEN] = {page_number, field, slot};
    const char *text = page_text(ctx(module), page_number, field, slot);
    size_t text_length = text ? strlen(text) : 0;
    if (text_length) {
        memcpy(page_data + 3, text, text_length);
    }
    tfbus_cmd_queue(module, 0x12, page_data, 3 + text_length);
}

/** @brief Stored text of PANEL_PAGE @p page_number (1..4) field / slot; NULL for fields without text. */
static char *page_text(sp4_t *panel_state, int page_number, uint8_t field, uint8_t slot)
{
    if (page_number < 1 || page_number > PAGES) {
        return NULL;
    }
    if (field == PAGE_FIELD_NAME && slot == 0) {
        return panel_state->page[page_number - 1].name;
    }
    if (slot < 1 || slot > SLOTS) {
        return NULL;
    }
    if (field == PAGE_FIELD_OBJECT_ID) {
        return panel_state->page[page_number - 1].object_id[slot - 1];
    }
    if (field == PAGE_FIELD_OBJECT_NAME) {
        return panel_state->page[page_number - 1].object_name[slot - 1];
    }
    return NULL;
}

/** @brief tfbus_driver_t::init: replay the captured two-pass init (see file description). */
static void init(tfbus_module_t *module)
{
    sp4_t *panel_state = ctx(module);

    // ---- pass 1: OM.LUA order
    for (int b = 0; b < BTN; b++) {
        send_button(module, b, false);
    }
    for (int i = 0; i < 4; i++) {
        send_icon(module, i);
    }
    for (size_t i = 0; i < NREGS; i++) {
        send_reg(module, i, panel_state->reg[i]);
    }
    // as after OM.LUA's set(1) Threshold (Sensitivity not set yet), then set(2) Sensitivity
    send_sensor_config(module, SENSOR_TEMP, SENSITIVITY_UNSET_MS);
    send_sensor_config(module, SENSOR_TEMP, panel_state->temp.sensitivity_ms);
    send_sensor_config(module, SENSOR_LIGHT, SENSITIVITY_UNSET_MS);
    send_sensor_config(module, SENSOR_LIGHT, panel_state->light.sensitivity_ms);
    for (uint8_t p = 1; p <= PAGES; p++) {
        send_page(module, p, PAGE_FIELD_TYPE, 0);
        send_page(module, p, PAGE_FIELD_NAME, 0);
        for (uint8_t s = 1; s <= SLOTS; s++) {
            send_page(module, p, PAGE_FIELD_OBJECT_ID, s);
            send_page(module, p, PAGE_FIELD_OBJECT_NAME, s);
        }
    }

    // ---- pass 2: CLU re-sends all features in reverse order
    for (uint8_t p = PAGES; p >= 1; p--) {
        send_page(module, p, PAGE_FIELD_TYPE, 0);
        send_page(module, p, PAGE_FIELD_NAME, 0);
        for (uint8_t s = 1; s <= SLOTS; s++) {
            send_page(module, p, PAGE_FIELD_OBJECT_NAME, s);
            send_page(module, p, PAGE_FIELD_OBJECT_ID, s);
            uint8_t button_index = (uint8_t)((p - 1) * SLOTS + s - 1);
            tfbus_cmd_queue(module, 0x1a, (uint8_t[]){button_index, 0x02, 0x00}, 3); // slot -> PANEL_BUTTON button_index
            send_page(module, p, PAGE_FIELD_CUSTOM_ICON, s);
        }
    }
    send_sensor_config(module, SENSOR_LIGHT, panel_state->light.sensitivity_ms);
    send_sensor_config(module, SENSOR_TEMP, panel_state->temp.sensitivity_ms);
    send_reg(module, 0, panel_state->reg[0]);
    send_reg(module, 1, panel_state->reg[1]);
    send_icon(module, 3); // right
    send_icon(module, 1); // down
    send_icon(module, 2); // left
    send_icon(module, 0); // up
    static const uint8_t pass2_regs[] = {3, 4, 2, REG_PAGENR, 6, 7, 8, 9};
    for (size_t i = 0; i < sizeof pass2_regs; i++) {
        size_t register_index = pass2_regs[i];
        // Captured: PageNr goes out as 0 here although OM.LUA set it to 1 (runtime current page?)
        send_reg(module, register_index, register_index == REG_PAGENR ? 0 : panel_state->reg[register_index]);
    }
    for (int b = BTN - 1; b >= 0; b--) {
        send_button(module, b, b == 0);
    }
    panel_state->initialized = true;
}

/**
 * @brief PANEL_PAGE set(): PageName (1), Object_n_Id (3, 5, 7, 9), Object_n_Name (4, 6, 8, 10) are
 *        stored and sent; other features are not decoded yet (warn once if set non-empty).
 */
static void page_set(tfbus_module_t *module, lua_State *L, int page_index, int idx, int val)
{
    sp4_t *panel_state = ctx(module);
    uint8_t field = 0, slot = 0;
    if (idx == 1) {
        field = PAGE_FIELD_NAME;
    } else if (idx >= 3 && idx <= 10) {
        field = idx % 2 ? PAGE_FIELD_OBJECT_ID : PAGE_FIELD_OBJECT_NAME;
        slot = (uint8_t)((idx - 1) / 2);
    }
    char *text = field ? page_text(panel_state, page_index + 1, field, slot) : NULL;
    if (text) {
        const char *value = lua_type(L, val) == LUA_TSTRING ? lua_tostring(L, val) : "";
        strlcpy(text, value, PAGE_TEXT_LEN + 1);
        if (panel_state->initialized) {
            send_page(module, (uint8_t)(page_index + 1), field, slot);
        }
        return;
    }
    bool empty = lua_type(L, val) == LUA_TSTRING ? lua_rawlen(L, val) == 0
                                                 : luaL_optinteger(L, val, 0) == 0;
    if (!empty && !panel_state->page_warned) {
        panel_state->page_warned = true;
        ESP_LOGW(TAG, "%s: PANEL_PAGE %d set(%d): page content encoding not decoded yet, not sent",
                 module->name, page_index + 1, idx);
    }
}

/** @brief tfbus_driver_t::io_new: remember the Lua name of a button / sensor / PANEL. */
static void io_new(tfbus_module_t *module, int cls, int channel_index, const char *name)
{
    sp4_t *panel_state = ctx(module);
    if (cls == CLS_BUTTON && channel_index >= 0 && channel_index < BTN) {
        strlcpy(panel_state->btn[channel_index], name, sizeof panel_state->btn[channel_index]);
    } else if (cls == CLS_PANEL && channel_index == 0) {
        strlcpy(panel_state->panel_name, name, sizeof panel_state->panel_name);
    } else if (cls == CLS_TEMP) {
        panel_sensor_name(&panel_state->temp, name);
    } else if (cls == CLS_LIGHT) {
        panel_sensor_name(&panel_state->light, name);
    }
}

/**
 * @brief tfbus_driver_t::io_set: button Mode, sensor limits, PANEL registers / icons (re-sent once initialised), pages.
 */
static bool io_set(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel_index, int idx, int val)
{
    sp4_t *panel_state = ctx(module);
    if (cls == CLS_PANEL) {
        if (idx >= 4 && idx <= 7) {
            int icon_index = idx - 4;
            const char *sensor_config = lua_type(L, val) == LUA_TSTRING ? lua_tostring(L, val) : "";
            strlcpy(panel_state->icon[icon_index], sensor_config, sizeof panel_state->icon[icon_index]);
            if (panel_state->initialized) {
                send_icon(module, icon_index);
            }
        }
        for (size_t i = 0; i < NREGS; i++) {
            if (k_regs[i].feature == idx) {
                panel_state->reg[i] = (uint16_t)luaL_optinteger(L, val, k_regs[i].def);
                if (panel_state->initialized) {
                    send_reg(module, i, panel_state->reg[i]);
                }
            }
        }
        return false; // keep generic storage so get() returns it
    }
    if (cls == CLS_PAGE) {
        page_set(module, L, channel_index, idx, val);
        return false;
    }
    if (cls == CLS_BUTTON && channel_index >= 0 && channel_index < BTN) {
        if (idx == 1) { // Mode / SetMode: the panel applies it
            lua_Integer configured_value = luaL_optinteger(L, val, 0);
            panel_state->mode[channel_index] =
                configured_value >= 0 && configured_value <= MODE_LOCKED ? (uint8_t)configured_value : MODE_MONOSTABLE;
            panel_state->pressed_at[channel_index] = 0; // a press in progress gets no click / hold
            if (panel_state->initialized) {
                send_mode(module, channel_index);
            }
        } else if (idx == 2) {
            panel_state->hold_delay[channel_index] = (uint16_t)luaL_optinteger(L, val, 1000);
        } else if (idx == 3) {
            panel_state->hold_interval[channel_index] = (uint16_t)luaL_optinteger(L, val, 100);
        }
        return false;
    }
    panel_sensor_t *sensor_config = cls == CLS_TEMP ? &panel_state->temp : cls == CLS_LIGHT ? &panel_state->light : NULL;
    if (sensor_config) {
        int32_t configured_value = drv_centi(luaL_optnumber(L, val, 0));
        int sensor_id = cls == CLS_TEMP ? SENSOR_TEMP : SENSOR_LIGHT;
        if (idx == 1) {
            sensor_config->threshold = configured_value;
        } else if (idx == 2) {
            lua_Integer sensitivity_ms = luaL_optinteger(L, val, 0);
            sensor_config->sensitivity_ms =
                (uint16_t)(sensitivity_ms < 0 ? 0 : sensitivity_ms > 0xffff ? 0xffff : sensitivity_ms);
        } else if (idx == 3) {
            sensor_config->min = configured_value;
        } else if (idx == 4) {
            sensor_config->max = configured_value;
        }
        if ((idx == 1 || idx == 2) && panel_state->initialized) { // the panel applies both
            send_sensor_config(module, sensor_id, sensor_config->sensitivity_ms);
        }
    }
    return false;
}

/** @brief Class frame 0x16 with the sensor config (panel_sensor_config()) and @p sensitivity_ms. */
static void send_sensor_config(tfbus_module_t *module, int sensor_id, uint16_t sensitivity_ms)
{
    uint8_t sensor_config[4];
    panel_sensor_config(sensor_id == SENSOR_TEMP ? &ctx(module)->temp : &ctx(module)->light, (uint8_t)sensor_id,
                        sensitivity_ms, sensor_config);
    drv_queue_frame(module, 0x16, sensor_config, sizeof sensor_config);
}

/**
 * @brief LED_CMD [mask_lo mask_hi cmd] for button @p button_index.
 *
 * Captures only show buttons 1-8 (mask_hi 0); mask_hi for buttons 9-16 is an assumption (the
 * RP2350 emulator reads a 16-bit mask).
 */
static void led(tfbus_module_t *module, int button_index, uint8_t cmd)
{
    uint16_t mask = (uint16_t)(1u << button_index);
    drv_queue_frame(module, 0x18, (uint8_t[]){(uint8_t)mask, (uint8_t)(mask >> 8), cmd}, 3);
}

/**
 * @brief PANEL methods -> TFBUS_CMD sub-commands (SwitchOnDisplay, ShowButtons, ClearScreen, ...).
 */
static int panel_execute(tfbus_module_t *module, lua_State *L, int idx, int first, int nargs)
{
    switch (idx) {
    case 0: // SwitchOnDisplay
        tfbus_cmd_queue(module, 0x00, NULL, 0);
        return 0;
    case 1: // ShowButtons
        tfbus_cmd_queue(module, 0x03, (uint8_t[]){0, 0, 0, 0}, 4);
        return 0;
    case 2: // ClearScreen
        tfbus_cmd_queue(module, 0x04, NULL, 0);
        return 0;
    case 3: { // PrintText(x, y, txt, fontSize)
        uint8_t text_data[19] = {0};
        text_data[0] = (uint8_t)luaL_optinteger(L, first, 0);
        text_data[1] = (uint8_t)luaL_optinteger(L, first + 1, 0);
        text_data[2] = (uint8_t)luaL_optinteger(L, first + 3, 1); // before tolstring: it pushes
        const char *txt = nargs > 2 ? luaL_tolstring(L, first + 2, NULL) : "";
        strncpy((char *)text_data + 3, txt, 16);
        tfbus_cmd_queue(module, 0x0a, text_data, sizeof text_data);
        return 0;
    }
    case 7: // DisplayContent: flush the freedraw buffer to the screen
        tfbus_cmd_queue(module, 0x09, NULL, 0);
        return 0;
    case 29: // Draw
        tfbus_cmd_queue(module, 0x19, NULL, 0);
        return 0;
    case 21: { // SetBeep(freq, duration, volume)
        uint16_t frequency = (uint16_t)luaL_optinteger(L, first, 1000);
        uint16_t duration = (uint16_t)luaL_optinteger(L, first + 1, 100);
        uint16_t volume = (uint16_t)luaL_optinteger(L, first + 2, 8);
        uint8_t text_data[7] = {(uint8_t)frequency, (uint8_t)(frequency >> 8), (uint8_t)duration, (uint8_t)(duration >> 8),
                        (uint8_t)volume, (uint8_t)(volume >> 8), 0};
        tfbus_cmd_queue(module, 0x0f, text_data, sizeof text_data);
        return 0;
    }
    default:
        ESP_LOGW(TAG, "PANEL execute(%d) not decoded yet", idx);
        return 0;
    }
}

/**
 * @brief tfbus_driver_t::io_execute: PANEL methods, PANEL_BUTTON ShowOK (0) / LedSwitchOn (1) / LedSwitchOff (2).
 */
static int io_execute(tfbus_module_t *module, lua_State *L, int obj, int cls, int channel_index, int idx, int first,
                      int nargs)
{
    if (cls == CLS_PANEL) {
        return panel_execute(module, L, idx, first, nargs);
    }
    if (cls != CLS_BUTTON || channel_index < 0 || channel_index >= BTN) {
        return -1;
    }
    switch (idx) {
    case 0: led(module, channel_index, LED_OK); return 0;     // ShowOK
    case 1: led(module, channel_index, LED_ON); return 0;     // LedSwitchOn
    case 2: led(module, channel_index, LED_OFF); return 0;    // LedSwitchOff
    default:
        ESP_LOGW(TAG, "%s: execute(%d) not decoded yet", ctx(module)->btn[channel_index], idx);
        return 0;
    }
}

static void button_edges(sp4_t *panel_state, uint16_t changed, uint16_t values, int64_t now);
static void button_holds(sp4_t *panel_state, int64_t now);
static void ext_events(const sp4_t *panel_state, const uint8_t *status_payload);

/**
 * @brief tfbus_driver_t::on_status: state frame (button Values, temperature, light, PANEL events) or TLV
 *        sensor records; every reply (a bare ACK too) also advances OnHold.
 */
static void on_status(tfbus_module_t *module, const uint8_t *status_payload, size_t len)
{
    sp4_t *panel_state = ctx(module);
    int64_t now = esp_timer_get_time();
    if (len == SENSOR_FRAME_LEN && status_payload[EXT_BYTE] & EXT_DISPLAY) {
        // display record: the other fields are zero, not buttons / temperature
        if (panel_state->panel_name[0]) {
            clu_lua_post_event(panel_state->panel_name, status_payload[0] ? PEV_DISPLAY_ON : PEV_DISPLAY_OFF);
        }
    } else if (len == SENSOR_FRAME_LEN) {
        uint16_t changed = (uint16_t)(status_payload[0] | status_payload[1] << 8);
        uint16_t values = (uint16_t)(status_payload[2] | status_payload[3] << 8);
        button_edges(panel_state, changed, values, now);
        int16_t temperature_deci = (int16_t)(status_payload[4] | status_payload[5] << 8);
        panel_sensor_update(&panel_state->temp, temperature_deci * 10);
        uint16_t light_deci = (uint16_t)(status_payload[6] | status_payload[7] << 8);
        panel_sensor_update(&panel_state->light, light_deci * 10);
        ext_events(panel_state, status_payload);
    } else {
        tfbus_tlv_t status_records[4];
        int status_record_count = tfbus_parse_tlv(status_payload, len, status_records, 4);
        for (int i = 0; i < status_record_count; i++) {
            if (status_records[i].len < 2) {
                continue;
            }
            uint16_t record_value = (uint16_t)(status_records[i].data[0] | status_records[i].data[1] << 8);
            switch (status_records[i].type) {
            case 0x02: panel_sensor_update(&panel_state->temp, (int16_t)record_value); break;
            case 0x07: panel_sensor_update(&panel_state->light, record_value); break; // raw ADC; % mapping unknown
            default: break;
            }
        }
    }
    button_holds(panel_state, now);
}

/** @brief PANEL gesture / proximity events from the ext flags of a state frame. */
static void ext_events(const sp4_t *panel_state, const uint8_t *status_payload)
{
    if (!panel_state->panel_name[0]) {
        return;
    }
    for (size_t i = 0; i < sizeof k_ext_events / sizeof k_ext_events[0]; i++) {
        if (status_payload[EXT_BYTE] & k_ext_events[i].flag) {
            clu_lua_post_event(panel_state->panel_name, k_ext_events[i].event);
        }
    }
}

/**
 * @brief Value changes of the buttons in @p changed -> Value, OnChange, OnSwitchOn / OnSwitchOff;
 *        a monostable release adds OnClick / OnShortPress / OnLongPress by press time (as DIN).
 *
 * Bistable buttons toggle on the panel, so the time between two reports is not a press time: they
 * get no click / hold events. Locked buttons report nothing.
 */
static void button_edges(sp4_t *panel_state, uint16_t changed, uint16_t values, int64_t now)
{
    for (int b = 0; b < BTN; b++) {
        uint16_t button_bit = (uint16_t)(1u << b);
        if (!(changed & button_bit)) {
            continue;
        }
        bool on = values & button_bit;
        bool was = panel_state->btn_value & button_bit;
        panel_state->btn_value = on ? (uint16_t)(panel_state->btn_value | button_bit)
                                    : (uint16_t)(panel_state->btn_value & ~button_bit);
        const char *name = panel_state->btn[b];
        if (on == was || !name[0]) {
            continue;
        }
        clu_lua_post_number(name, 0, on);
        clu_lua_post_event(name, EV_CHANGE);
        clu_lua_post_event(name, on ? EV_ON : EV_OFF);
        if (panel_state->mode[b] != MODE_MONOSTABLE) {
            continue;
        }
        if (on) {
            panel_state->pressed_at[b] = now;
            panel_state->next_hold[b] = now + (int64_t)panel_state->hold_delay[b] * 1000;
        } else if (panel_state->pressed_at[b]) {
            int64_t held_ms = (now - panel_state->pressed_at[b]) / 1000;
            clu_lua_post_event(name, held_ms < 500 ? EV_CLICK : held_ms < 2000 ? EV_SHORT : EV_LONG);
            panel_state->pressed_at[b] = 0;
        }
    }
}

/** @brief OnHold after HoldDelay, then every HoldInterval, while a monostable button is held. */
static void button_holds(sp4_t *panel_state, int64_t now)
{
    for (int b = 0; b < BTN; b++) {
        if (panel_state->pressed_at[b] && now >= panel_state->next_hold[b]) {
            clu_lua_post_event(panel_state->btn[b], EV_HOLD);
            panel_state->next_hold[b] = now + (int64_t)panel_state->hold_interval[b] * 1000;
        }
    }
}

const tfbus_driver_t tfbus_drv_smart_panel_fm4 = {
    .hw_type = 0x03,
    .name = "SMART_PANEL_FM_4",
    .init = init,
    .on_status = on_status,
    .io_new = io_new,
    .io_set = io_set,
    .io_execute = io_execute,
};
