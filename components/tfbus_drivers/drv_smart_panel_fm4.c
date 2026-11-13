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

static void led(tfbus_module_t *m, int b, uint8_t cmd);

/** @brief Driver state of @p m, allocated (with sensor / register defaults) on first use. */
static sp4_t *ctx(tfbus_module_t *m)
{
    if (!m->drv_ctx) {
        sp4_t *c = calloc(1, sizeof *c);
        if (!c) {
            abort(); // out of memory while loading OM.LUA: reboot instead of a NULL dereference
        }
        c->temp = (panel_sensor_t){.threshold = 0.1, .min = 0, .max = 45};
        c->light = (panel_sensor_t){.threshold = 0.5, .min = 0, .max = 100};
        for (size_t i = 0; i < NREGS; i++) {
            c->reg[i] = k_regs[i].def;
        }
        m->drv_ctx = c;
    }
    return m->drv_ctx;
}

/** @brief Queue a TFBUS_CMD (0x31) frame with sub-command @p cmd. */
static void tfbus_cmd_queue(tfbus_module_t *m, uint8_t cmd, const uint8_t *data, size_t len)
{
    uint8_t f[TFBUS_MAX_FRAME];
    size_t n = tfbus_cmd(f, sizeof f, m->addr, cmd, data, len);
    if (n) {
        tfbus_master_queue(f, n);
    }
}

/** @brief Send PANEL register k_regs[@p i] = @p v. */
static void send_reg(tfbus_module_t *m, size_t i, uint16_t v)
{
    tfbus_cmd_queue(m, k_regs[i].reg, (uint8_t[]){(uint8_t)v, (uint8_t)(v >> 8)}, 2);
}

/** @brief Send gesture icon @p i as a DISPLAY_BLOCK (0x30). */
static void send_icon(tfbus_module_t *m, int i)
{
    uint8_t blk[22] = {0, k_icon_sub[i]};
    memcpy(blk + 2, ctx(m)->icon[i], strlen(ctx(m)->icon[i]));
    drv_queue_frame(m, 0x30, blk, sizeof blk);
}

/**
 * @brief Send one button's class frame (0x17) and DISPLAY_BLOCKs; optionally the LED commit (pass 2).
 */
static void send_button(tfbus_module_t *m, int b, bool led_commit)
{
    uint16_t mask = (uint16_t)(1u << b);
    drv_queue_frame(m, 0x17, (uint8_t[]){(uint8_t)mask, (uint8_t)(mask >> 8), 0}, 3);
    if (led_commit) {
        drv_queue_frame(m, 0x18, (uint8_t[]){0x00, 0x00, 0x02}, 3);
    }
    static const uint8_t subs[3] = {0x0a, 0x00, 0x01};
    for (int i = 0; i < 3; i++) {
        uint8_t blk[22] = {(uint8_t)b, subs[i]};
        drv_queue_frame(m, 0x30, blk, sizeof blk);
    }
}

/** @brief Send a PANEL_PAGE field header (TFBUS_CMD 0x12 [p, field, slot]). */
static void send_page(tfbus_module_t *m, uint8_t p, uint8_t field, uint8_t slot)
{
    tfbus_cmd_queue(m, 0x12, (uint8_t[]){p, field, slot}, 3);
}

/** @brief tfbus_driver_t::init: replay the captured two-pass init (see file description). */
static void init(tfbus_module_t *m)
{
    sp4_t *c = ctx(m);

    // ---- pass 1: OM.LUA order
    for (int b = 0; b < BTN; b++) {
        send_button(m, b, false);
    }
    for (int i = 0; i < 4; i++) {
        send_icon(m, i);
    }
    for (size_t i = 0; i < NREGS; i++) {
        send_reg(m, i, c->reg[i]);
    }
    drv_queue_frame(m, 0x16, (uint8_t[]){0x00, 0x01, 0x02, 0}, 4);
    drv_queue_frame(m, 0x16, (uint8_t[]){0x00, 0x0a, 0x02, 0}, 4);
    drv_queue_frame(m, 0x16, (uint8_t[]){0x01, 0x01, 0x0a, 0}, 4);
    drv_queue_frame(m, 0x16, (uint8_t[]){0x01, 0x0a, 0x0a, 0}, 4);
    for (uint8_t p = 1; p <= PAGES; p++) {
        send_page(m, p, 0x00, 0); // PageType
        send_page(m, p, 0x01, 0); // PageName
        for (uint8_t s = 1; s <= SLOTS; s++) {
            send_page(m, p, 0x03, s); // Object_s_Id
            send_page(m, p, 0x04, s); // Object_s_Name
        }
    }

    // ---- pass 2: CLU re-sends all features in reverse order
    for (uint8_t p = PAGES; p >= 1; p--) {
        send_page(m, p, 0x00, 0);
        send_page(m, p, 0x01, 0);
        for (uint8_t s = 1; s <= SLOTS; s++) {
            send_page(m, p, 0x04, s);
            send_page(m, p, 0x03, s);
            uint8_t b = (uint8_t)((p - 1) * SLOTS + s - 1);
            tfbus_cmd_queue(m, 0x1a, (uint8_t[]){b, 0x02, 0x00}, 3); // slot -> PANEL_BUTTON b
            send_page(m, p, 0x06, s);
        }
    }
    drv_queue_frame(m, 0x16, (uint8_t[]){0x01, 0x0a, 0x0a, 0}, 4);
    drv_queue_frame(m, 0x16, (uint8_t[]){0x00, 0x0a, 0x02, 0}, 4);
    send_reg(m, 0, c->reg[0]);
    send_reg(m, 1, c->reg[1]);
    send_icon(m, 3); // right
    send_icon(m, 1); // down
    send_icon(m, 2); // left
    send_icon(m, 0); // up
    static const uint8_t pass2_regs[] = {3, 4, 2, REG_PAGENR, 6, 7, 8, 9};
    for (size_t i = 0; i < sizeof pass2_regs; i++) {
        size_t r = pass2_regs[i];
        // Captured: PageNr goes out as 0 here although OM.LUA set it to 1 (runtime current page?)
        send_reg(m, r, r == REG_PAGENR ? 0 : c->reg[r]);
    }
    for (int b = BTN - 1; b >= 0; b--) {
        send_button(m, b, b == 0);
    }
    c->initialized = true;
}

/**
 * @brief PANEL_PAGE set(): content encoding is not decoded yet; warn once if non-empty content is set.
 */
static void page_set(tfbus_module_t *m, lua_State *L, int ch, int idx, int val)
{
    sp4_t *c = ctx(m);
    bool empty = lua_type(L, val) == LUA_TSTRING ? lua_rawlen(L, val) == 0
                                                 : luaL_optinteger(L, val, 0) == 0;
    if (!empty && !c->page_warned) {
        c->page_warned = true;
        ESP_LOGW(TAG, "%s: PANEL_PAGE %d set(%d): page content encoding not decoded yet, not sent",
                 m->name, ch + 1, idx);
    }
}

/** @brief tfbus_driver_t::io_new: remember the Lua name of a button / sensor. */
static void io_new(tfbus_module_t *m, int cls, int ch, const char *name)
{
    sp4_t *c = ctx(m);
    if (cls == CLS_BUTTON && ch >= 0 && ch < BTN) {
        strlcpy(c->btn[ch], name, sizeof c->btn[ch]);
    } else if (cls == CLS_TEMP) {
        strlcpy(c->temp.name, name, sizeof c->temp.name);
    } else if (cls == CLS_LIGHT) {
        strlcpy(c->light.name, name, sizeof c->light.name);
    }
}

/**
 * @brief tfbus_driver_t::io_set: button Mode, sensor limits, PANEL registers / icons (re-sent once initialised), pages.
 */
static bool io_set(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int val)
{
    sp4_t *c = ctx(m);
    if (cls == CLS_PANEL) {
        if (idx >= 4 && idx <= 7) {
            int i = idx - 4;
            const char *s = lua_type(L, val) == LUA_TSTRING ? lua_tostring(L, val) : "";
            strlcpy(c->icon[i], s, sizeof c->icon[i]);
            if (c->initialized) {
                send_icon(m, i);
            }
        }
        for (size_t i = 0; i < NREGS; i++) {
            if (k_regs[i].feature == idx) {
                c->reg[i] = (uint16_t)luaL_optinteger(L, val, k_regs[i].def);
                if (c->initialized) {
                    send_reg(m, i, c->reg[i]);
                }
            }
        }
        return false; // keep generic storage so get() returns it
    }
    if (cls == CLS_PAGE) {
        page_set(m, L, ch, idx, val);
        return false;
    }
    if (cls == CLS_BUTTON && ch >= 0 && ch < BTN && idx == 1) { // Mode / SetMode
        lua_Integer v = luaL_optinteger(L, val, 0);
        c->mode[ch] = v >= 0 && v <= 2 ? (uint8_t)v : 0;
        if (c->mode[ch] != PANEL_MODE_BISTABLE && (c->btn_value & (1u << ch))) {
            c->btn_value &= ~(1u << ch);
            if (c->initialized) {
                led(m, ch, LED_OFF);
            }
        }
        return false;
    }
    panel_sensor_t *s = cls == CLS_TEMP ? &c->temp : cls == CLS_LIGHT ? &c->light : NULL;
    if (s) {
        double v = luaL_optnumber(L, val, 0);
        if (idx == 1) {
            s->threshold = v;
        } else if (idx == 3) {
            s->min = v;
        } else if (idx == 4) {
            s->max = v;
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
static void led(tfbus_module_t *m, int b, uint8_t cmd)
{
    uint16_t mask = (uint16_t)(1u << b);
    drv_queue_frame(m, 0x18, (uint8_t[]){(uint8_t)mask, (uint8_t)(mask >> 8), cmd}, 3);
}

/**
 * @brief PANEL methods -> TFBUS_CMD sub-commands (SwitchOnDisplay, ShowButtons, ClearScreen, ...).
 */
static int panel_execute(tfbus_module_t *m, lua_State *L, int idx, int first, int nargs)
{
    switch (idx) {
    case 0: // SwitchOnDisplay
        tfbus_cmd_queue(m, 0x00, NULL, 0);
        return 0;
    case 1: // ShowButtons
        tfbus_cmd_queue(m, 0x03, (uint8_t[]){0, 0, 0, 0}, 4);
        return 0;
    case 2: // ClearScreen
        tfbus_cmd_queue(m, 0x04, NULL, 0);
        return 0;
    case 3: { // PrintText(x, y, txt, fontSize)
        uint8_t d[19] = {0};
        d[0] = (uint8_t)luaL_optinteger(L, first, 0);
        d[1] = (uint8_t)luaL_optinteger(L, first + 1, 0);
        d[2] = (uint8_t)luaL_optinteger(L, first + 3, 1); // before tolstring: it pushes
        const char *txt = nargs > 2 ? luaL_tolstring(L, first + 2, NULL) : "";
        strncpy((char *)d + 3, txt, 16);
        tfbus_cmd_queue(m, 0x0a, d, sizeof d);
        return 0;
    }
    case 7: // DisplayContent: flush the freedraw buffer to the screen
        tfbus_cmd_queue(m, 0x09, NULL, 0);
        return 0;
    case 29: // Draw
        tfbus_cmd_queue(m, 0x19, NULL, 0);
        return 0;
    case 21: { // SetBeep(freq, duration, volume)
        uint16_t fq = (uint16_t)luaL_optinteger(L, first, 1000);
        uint16_t du = (uint16_t)luaL_optinteger(L, first + 1, 100);
        uint16_t vo = (uint16_t)luaL_optinteger(L, first + 2, 8);
        uint8_t d[7] = {(uint8_t)fq, (uint8_t)(fq >> 8), (uint8_t)du, (uint8_t)(du >> 8),
                        (uint8_t)vo, (uint8_t)(vo >> 8), 0};
        tfbus_cmd_queue(m, 0x0f, d, sizeof d);
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
static int io_execute(tfbus_module_t *m, lua_State *L, int obj, int cls, int ch, int idx, int first,
                      int nargs)
{
    if (cls == CLS_PANEL) {
        return panel_execute(m, L, idx, first, nargs);
    }
    if (cls != CLS_BUTTON || ch < 0 || ch >= BTN) {
        return -1;
    }
    switch (idx) {
    case 0: led(m, ch, LED_OK); return 0;     // ShowOK
    case 1: led(m, ch, LED_ON); return 0;     // LedSwitchOn
    case 2: led(m, ch, LED_OFF); return 0;    // LedSwitchOff
    default:
        ESP_LOGW(TAG, "%s: execute(%d) not decoded yet", ctx(m)->btn[ch], idx);
        return 0;
    }
}

/**
 * @brief tfbus_driver_t::on_status: temperature, light and button event records (button Mode applied here).
 */
static void on_status(tfbus_module_t *m, const uint8_t *pl, size_t len)
{
    sp4_t *c = ctx(m);
    tfbus_tlv_t r[4];
    int n = tfbus_parse_tlv(pl, len, r, 4);
    for (int i = 0; i < n; i++) {
        if (r[i].len < 2) {
            continue;
        }
        uint16_t v = (uint16_t)(r[i].data[0] | r[i].data[1] << 8);
        switch (r[i].type) {
        case 0x02: panel_sensor_update(&c->temp, (int16_t)v / 100.0); break;
        case 0x07: panel_sensor_update(&c->light, v / 100.0); break; // raw ADC; % mapping unknown
        case 0x08: {
            uint32_t before = c->btn_value;
            panel_button_event(c->btn, BTN, &c->btn_value, c->mode, r[i].data[0]);
            // Bistable: the LED shows the toggled state (green on, off).
            uint32_t flipped = before ^ c->btn_value;
            for (int b = 0; b < BTN; b++) {
                if ((flipped & (1u << b)) && c->mode[b] == PANEL_MODE_BISTABLE) {
                    led(m, b, (c->btn_value & (1u << b)) ? LED_ON : LED_OFF);
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
