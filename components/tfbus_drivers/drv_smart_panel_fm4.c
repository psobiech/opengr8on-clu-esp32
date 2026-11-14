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
 *     sensors: class 0x16 [00,01,02,00] [00,0a,02,00] [01,01,0a,00] [01,0a,0a,00]
 *       (sent verbatim: byte 1 is NOT the threshold - OM sets light threshold 0.5, wire has 01)
 *     PANEL_PAGE p=1..4 set(0,1,3..10): 0x31 [12, p, field, slot] field 00/01 header, 03/04 per slot
 *   pass 2 (reverse): pages 4..1 incl. 0x31 [1a, button, 02, 00] + [12, p, 06, slot];
 *     sensors 2 of 4; regs with PageNr=0; icons; buttons 15..0 with LED_CMD 0x18 [00,00,02]
 *     right before button 0's blocks.
 * @endverbatim
 * NOT YET DECODED: how non-empty labels/icons/page content are encoded (only headers are sent).
 *
 * Runtime: LED_CMD 0x18 [mask, 0, cmd]; TFBUS_CMD (PANEL execute -> sub-cmd): SwitchOnDisplay 0x00,
 * ShowButtons 0x03 [0,0,0,0], ClearScreen 0x04, PrintText 0x0a, DisplayContent 0x09, Draw 0x19,
 * SetBeep 0x0f; PANEL set(4..7, 10..21) re-sends its frame.
 *
 * Status: as touch panel (06 baseline, 02 temp, 07 light raw, 08 button VAL).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clu_lua.h"
#include "drv_panel_common.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "tfbus_drivers.h"
#include "tfbus_master.h"

static const char *TAG = "drv_sp4";

#define BTN 16
/** @brief OM object classes hosted on the panel. */
enum { CLS_PAGE = 19, CLS_TEMP = 20, CLS_LIGHT = 21, CLS_PANEL = 29, CLS_BUTTON = 30 };
/** @brief LED_CMD command codes. */
enum { LED_OK = 0x00, LED_ERROR = 0x01, LED_ON = 0x02, LED_OFF = 0x04, LED_RED_ON = 0x10 };

#define PAGES 4
#define SLOTS 4
#define ICON_LEN 20

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

/** @brief Driver state per module (drv_ctx). */
typedef struct {
    char btn[BTN][24];
    uint32_t btn_value;
    uint8_t mode[BTN]; // PANEL_BUTTON Mode (feature 1), applied by the CLU (wire format unknown)
    panel_sensor_t temp, light;
    uint16_t reg[NREGS];
    char icon[4][ICON_LEN + 1];
    bool initialized;
    bool page_warned;
} sp4_t;

static void led(tfbus_module_t *module, int button_index, uint8_t cmd);

/** @brief Driver state of @p m, allocated (with sensor / register defaults) on first use. */
static sp4_t *ctx(tfbus_module_t *module)
{
    if (!module->drv_ctx) {
        sp4_t *panel_state = calloc(1, sizeof *panel_state);
        if (!panel_state) {
            abort(); // out of memory while loading OM.LUA: reboot instead of a NULL dereference
        }
        panel_state->temp = (panel_sensor_t){.threshold = 0.1, .min = 0, .max = 45};
        panel_state->light = (panel_sensor_t){.threshold = 0.5, .min = 0, .max = 100};
        for (size_t i = 0; i < NREGS; i++) {
            panel_state->reg[i] = k_regs[i].def;
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
    uint16_t mask = (uint16_t)(1u << button_index);
    drv_queue_frame(module, 0x17, (uint8_t[]){(uint8_t)mask, (uint8_t)(mask >> 8), 0}, 3);
    if (led_commit) {
        drv_queue_frame(module, 0x18, (uint8_t[]){0x00, 0x00, 0x02}, 3);
    }
    static const uint8_t subs[3] = {0x0a, 0x00, 0x01};
    for (int i = 0; i < 3; i++) {
        uint8_t blk[22] = {(uint8_t)button_index, subs[i]};
        drv_queue_frame(module, 0x30, blk, sizeof blk);
    }
}

/** @brief Send a PANEL_PAGE field header (TFBUS_CMD 0x12 [p, field, slot]). */
static void send_page(tfbus_module_t *module, uint8_t page_number, uint8_t field, uint8_t slot)
{
    tfbus_cmd_queue(module, 0x12, (uint8_t[]){page_number, field, slot}, 3);
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
    drv_queue_frame(module, 0x16, (uint8_t[]){0x00, 0x01, 0x02, 0}, 4);
    drv_queue_frame(module, 0x16, (uint8_t[]){0x00, 0x0a, 0x02, 0}, 4);
    drv_queue_frame(module, 0x16, (uint8_t[]){0x01, 0x01, 0x0a, 0}, 4);
    drv_queue_frame(module, 0x16, (uint8_t[]){0x01, 0x0a, 0x0a, 0}, 4);
    for (uint8_t p = 1; p <= PAGES; p++) {
        send_page(module, p, 0x00, 0); // PageType
        send_page(module, p, 0x01, 0); // PageName
        for (uint8_t s = 1; s <= SLOTS; s++) {
            send_page(module, p, 0x03, s); // Object_s_Id
            send_page(module, p, 0x04, s); // Object_s_Name
        }
    }

    // ---- pass 2: CLU re-sends all features in reverse order
    for (uint8_t p = PAGES; p >= 1; p--) {
        send_page(module, p, 0x00, 0);
        send_page(module, p, 0x01, 0);
        for (uint8_t s = 1; s <= SLOTS; s++) {
            send_page(module, p, 0x04, s);
            send_page(module, p, 0x03, s);
            uint8_t b = (uint8_t)((p - 1) * SLOTS + s - 1);
            tfbus_cmd_queue(module, 0x1a, (uint8_t[]){b, 0x02, 0x00}, 3); // slot -> PANEL_BUTTON b
            send_page(module, p, 0x06, s);
        }
    }
    drv_queue_frame(module, 0x16, (uint8_t[]){0x01, 0x0a, 0x0a, 0}, 4);
    drv_queue_frame(module, 0x16, (uint8_t[]){0x00, 0x0a, 0x02, 0}, 4);
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
 * @brief PANEL_PAGE set(): content encoding is not decoded yet; warn once if non-empty content is set.
 */
static void page_set(tfbus_module_t *module, lua_State *L, int page_index, int idx, int val)
{
    sp4_t *panel_state = ctx(module);
    bool empty = lua_type(L, val) == LUA_TSTRING ? lua_rawlen(L, val) == 0
                                                 : luaL_optinteger(L, val, 0) == 0;
    if (!empty && !panel_state->page_warned) {
        panel_state->page_warned = true;
        ESP_LOGW(TAG, "%s: PANEL_PAGE %d set(%d): page content encoding not decoded yet, not sent",
                 module->name, page_index + 1, idx);
    }
}

/** @brief tfbus_driver_t::io_new: remember the Lua name of a button / sensor. */
static void io_new(tfbus_module_t *module, int cls, int channel_index, const char *name)
{
    sp4_t *panel_state = ctx(module);
    if (cls == CLS_BUTTON && channel_index >= 0 && channel_index < BTN) {
        strlcpy(panel_state->btn[channel_index], name, sizeof panel_state->btn[channel_index]);
    } else if (cls == CLS_TEMP) {
        strlcpy(panel_state->temp.name, name, sizeof panel_state->temp.name);
    } else if (cls == CLS_LIGHT) {
        strlcpy(panel_state->light.name, name, sizeof panel_state->light.name);
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
            int i = idx - 4;
            const char *sensor_config = lua_type(L, val) == LUA_TSTRING ? lua_tostring(L, val) : "";
            strlcpy(panel_state->icon[i], sensor_config, sizeof panel_state->icon[i]);
            if (panel_state->initialized) {
                send_icon(module, i);
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
    if (cls == CLS_BUTTON && channel_index >= 0 && channel_index < BTN && idx == 1) { // Mode / SetMode
        lua_Integer configured_value = luaL_optinteger(L, val, 0);
        panel_state->mode[channel_index] = configured_value >= 0 && configured_value <= 2 ? (uint8_t)configured_value : 0;
        if (panel_state->mode[channel_index] != PANEL_MODE_BISTABLE && (panel_state->btn_value & (1u << channel_index))) {
            panel_state->btn_value &= ~(1u << channel_index);
            if (panel_state->initialized) {
                led(module, channel_index, LED_OFF);
            }
        }
        return false;
    }
    panel_sensor_t *sensor_config = cls == CLS_TEMP ? &panel_state->temp : cls == CLS_LIGHT ? &panel_state->light : NULL;
    if (sensor_config) {
        double configured_value = luaL_optnumber(L, val, 0);
        if (idx == 1) {
            sensor_config->threshold = configured_value;
        } else if (idx == 3) {
            sensor_config->min = configured_value;
        } else if (idx == 4) {
            sensor_config->max = configured_value;
        }
    }
    return false;
}

/**
 * @brief LED_CMD [mask_lo mask_hi cmd] for button @p b.
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

/**
 * @brief tfbus_driver_t::on_status: temperature, light and button event records (button Mode applied here).
 */
static void on_status(tfbus_module_t *module, const uint8_t *status_payload, size_t len)
{
    sp4_t *panel_state = ctx(module);
    tfbus_tlv_t status_records[4];
    int status_record_count = tfbus_parse_tlv(status_payload, len, status_records, 4);
    for (int i = 0; i < status_record_count; i++) {
        if (status_records[i].len < 2) {
            continue;
        }
        uint16_t record_value = (uint16_t)(status_records[i].data[0] | status_records[i].data[1] << 8);
        switch (status_records[i].type) {
        case 0x02: panel_sensor_update(&panel_state->temp, (int16_t)record_value / 100.0); break;
        case 0x07: panel_sensor_update(&panel_state->light, record_value / 100.0); break; // raw ADC; % mapping unknown
        case 0x08: {
            uint32_t before = panel_state->btn_value;
            panel_button_event(panel_state->btn, BTN, &panel_state->btn_value, panel_state->mode, status_records[i].data[0]);
            // Bistable: the LED shows the toggled state (green on, off).
            uint32_t flipped = before ^ panel_state->btn_value;
            for (int b = 0; b < BTN; b++) {
                if ((flipped & (1u << b)) && panel_state->mode[b] == PANEL_MODE_BISTABLE) {
                    led(module, b, (panel_state->btn_value & (1u << b)) ? LED_ON : LED_OFF);
                }
            }
            break;
        }
        default: break;
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
