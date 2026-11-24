// Host tests for the status OLED: the SSD1306 driver glue (clu_display.c) against a fake esp_lcd
// panel and I2C bus, and the status screen (main/clu_status_display.c) against fake network,
// clock, uptime and TF-Bus registry. Sources included: their state is static.
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONFIG_CLU_DISPLAY          1
#define CONFIG_CLU_DISPLAY_SDA_GPIO 8
#define CONFIG_CLU_DISPLAY_SCL_GPIO 18
#define CONFIG_CLU_DISPLAY_ADDR     0x3c
#define CONFIG_CLU_ETH              1
#define CONFIG_CLU_WIFI             1
#define CONFIG_CLU_TFBUS_MASTER     1

#define TAG I2C_TAG // both sources define TAG
#include "../../components/clu_i2c/clu_i2c.c"
#undef TAG
#include "../../components/clu_display/clu_display.c"

static time_t host_time(time_t *timer);
#define time(timer) host_time(timer) // the status screen reads the fake clock
#include "../../main/clu_status_display.c"
#undef time

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- fakes: state
typedef enum {
    FAIL_NONE,
    FAIL_BUS,
    FAIL_PANEL_IO,
    FAIL_SSD1306,
    FAIL_RESET,
    FAIL_INIT,
    FAIL_MIRROR,
    FAIL_DISPLAY_ON,
} fail_step_t;

static struct host_i2c_bus { int sda_gpio, scl_gpio; } s_fake_bus;
static struct esp_lcd_panel_io_t { int unused; } s_fake_panel_io;
static struct esp_lcd_panel_t { int unused; } s_fake_panel;

static fail_step_t s_fail_step;
static int s_chip_address = -1; // address the fake SSD1306 answers at, -1: no chip
static esp_lcd_panel_io_i2c_config_t s_last_io_config;
static esp_lcd_panel_ssd1306_config_t s_last_ssd1306_config;
static esp_lcd_panel_dev_config_t s_last_panel_config;
static int s_panel_io_deletes, s_panel_deletes, s_panel_creates;
static bool s_mirror_x, s_mirror_y, s_display_on;

static int s_draw_count;
static bool s_draw_fails;
static int s_last_draw_x_start, s_last_draw_y_start, s_last_draw_x_end, s_last_draw_y_end;
static uint8_t s_screen_pages[CLU_DISPLAY_ROWS][DISPLAY_TEXT_WIDTH]; // what the fake panel shows

static void (*s_task_function)(void *);
static const char *s_task_name;
static uint32_t s_task_stack_size;
static UBaseType_t s_task_priority;
static int s_task_creates;
static jmp_buf s_task_exit;
static int s_delays_left; // vTaskDelay leaves the task loop when this reaches 0
static TickType_t s_last_delay_ticks;

clu_state_t g_clu;
static uint32_t s_interface_ips[CLU_NET_IF_COUNT];
static time_t s_now;
static int64_t s_uptime_us;
static tfbus_module_t s_modules[3];
static tfbus_module_t *s_module_slots[3]; // registry slots, NULL: dropped by a refresh

static void test_display_driver(void);
static void test_status_rows(void);
static void test_status_task(void);
static bool screen_row_shows(int row, const char *text);

int main(void)
{
    setenv("TZ", "UTC0", 1);
    tzset();
    test_display_driver();
    test_status_rows();
    test_status_task();
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- clu_display.c
static void test_display_driver(void)
{
    const char *const blank_rows[CLU_DISPLAY_ROWS] = {"", "", "", "", "", "", "", ""};

    clu_display_show(blank_rows);
    check("show before init draws nothing", s_draw_count == 0);

    clu_status_display_start();
    check("no display: status task not started", s_task_creates == 0);

    check("init with no bus pins fails", !clu_display_init(-1, 18, 0x3c));

    s_fail_step = FAIL_BUS;
    check("init fails when the I2C bus cannot be created", !clu_display_init(4, 5, 0x3c));

    s_fail_step = FAIL_NONE;
    s_chip_address = 0x3d;
    check("init fails when nothing answers at the address", !clu_display_init(8, 18, 0x3c));

    s_chip_address = 0x3c;
    s_fail_step = FAIL_PANEL_IO;
    check("init fails when the panel IO cannot be created", !clu_display_init(8, 18, 0x3c));
    check("panel IO config: address, 400 kHz", s_last_io_config.dev_addr == 0x3c &&
                                                    s_last_io_config.scl_speed_hz == 400000);
    check("panel IO config: control byte with the D/C# bit 6", s_last_io_config.control_phase_bytes == 1 &&
                                                                     s_last_io_config.dc_bit_offset == 6);
    check("panel IO config: 8-bit commands and parameters", s_last_io_config.lcd_cmd_bits == 8 &&
                                                                 s_last_io_config.lcd_param_bits == 8);

    s_fail_step = FAIL_SSD1306;
    check("init fails when the SSD1306 panel cannot be created", !clu_display_init(8, 18, 0x3c));
    check("panel IO deleted after the panel failed", s_panel_io_deletes == 1 && s_panel_deletes == 0);
    check("panel config: 1 bit per pixel, no reset pin, 64 rows",
          s_last_panel_config.bits_per_pixel == 1 && s_last_panel_config.reset_gpio_num == -1 &&
              s_last_ssd1306_config.height == 64);

    const fail_step_t setup_steps[] = {FAIL_RESET, FAIL_INIT, FAIL_MIRROR, FAIL_DISPLAY_ON};
    for (size_t step_index = 0; step_index < sizeof setup_steps / sizeof setup_steps[0]; step_index++) {
        s_fail_step = setup_steps[step_index];
        char name[64];
        snprintf(name, sizeof name, "init fails when setup step %d fails", (int)s_fail_step);
        check(name, !clu_display_init(8, 18, 0x3c));
    }
    check("panel and panel IO deleted after each failed setup", s_panel_deletes == 4 && s_panel_io_deletes == 5);
    clu_display_show(blank_rows);
    check("show after failed init draws nothing", s_draw_count == 0);

    s_fail_step = FAIL_NONE;
    clu_status_display_start();
    check("status display started: task created", s_task_creates == 1 && s_task_function == status_display_task);
    check("status task: name, stack, low priority", strcmp(s_task_name, "display") == 0 &&
                                                        s_task_stack_size == 3072 && s_task_priority == 1);
    check("panel mirrored on both axes (upright)", s_mirror_x && s_mirror_y);
    check("display switched on", s_display_on);
    check("display at the configured address", s_last_io_config.dev_addr == CONFIG_CLU_DISPLAY_ADDR);

    const int panel_creates_before = s_panel_creates;
    check("second init succeeds without setting up again",
          clu_display_init(8, 18, 0x3c) && s_panel_creates == panel_creates_before);

    clu_display_show(blank_rows);
    check("first show sends every row", s_draw_count == CLU_DISPLAY_ROWS);
    check("last row: full width, one 8-pixel page", s_last_draw_x_start == 0 && s_last_draw_x_end == 128 &&
                                                         s_last_draw_y_start == 56 && s_last_draw_y_end == 64);

    s_draw_count = 0;
    clu_display_show(blank_rows);
    check("unchanged rows are not sent again", s_draw_count == 0);

    const char *const one_changed_row[CLU_DISPLAY_ROWS] = {"", "", "Hello", "", "", "", "", ""};
    clu_display_show(one_changed_row);
    check("only the changed row is sent", s_draw_count == 1 && s_last_draw_y_start == 16 && s_last_draw_y_end == 24);
    check("changed row shows its text", screen_row_shows(2, "Hello"));

    const char *const another_change[CLU_DISPLAY_ROWS] = {"", "", "World", "", "", "", "", ""};
    s_draw_count = 0;
    s_draw_fails = true;
    clu_display_show(another_change);
    s_draw_fails = false;
    clu_display_show(another_change);
    check("row whose transfer failed is sent again", s_draw_count == 2 && screen_row_shows(2, "World"));

    s_draw_count = 0;
    s_draw_fails = true;
    clu_display_show(one_changed_row);
    s_draw_fails = false;
    clu_display_show(another_change);
    check("failed row resent even when the text is back to what was shown", s_draw_count == 2);
}

// ---------------------------------------------------------------- clu_status_display.c rows
static void test_status_rows(void)
{
    static status_rows_t rows;

    g_clu.sn = 123456789;
    s_interface_ips[CLU_NET_ETH] = 0x0101a8c0; // 192.168.1.1, network byte order
    s_interface_ips[CLU_NET_WIFI] = 0;
    s_now = 1735689600 + 3600 + 2 * 60 + 3; // 2025-01-01 01:02:03 UTC
    s_uptime_us = (int64_t)(2 * 86400 + 3 * 3600 + 4 * 60 + 5) * 1000000;
    s_modules[0].online = true;
    s_modules[1].online = false;
    s_modules[2].online = true;
    s_module_slots[0] = &s_modules[0];
    s_module_slots[1] = &s_modules[1];
    s_module_slots[2] = NULL; // dropped by a refresh: not counted
    memset(rows, 'x', sizeof rows);

    compose_rows(rows);
    check("row 0: serial", strcmp(rows[0], "CLU 123456789") == 0);
    check("row 1: Ethernet address", strcmp(rows[1], "ETH 192.168.1.1") == 0);
    check("row 2: WiFi without address", strcmp(rows[2], "WiFi -") == 0);
    check("row 3: blank separator", rows[3][0] == '\0');
    check("row 4: local date and time", strcmp(rows[4], "2025-01-01 01:02:03") == 0);
    check("row 5: TF-Bus modules online / declared", strcmp(rows[5], "TF-Bus 1/2 online") == 0);
    check("row 6: uptime", strcmp(rows[6], "Up 2d 03:04:05") == 0);
    check("row 7: blank", rows[7][0] == '\0');

    s_now = CLU_RTC_MIN_VALID - 1;
    s_uptime_us = 59 * 1000000;
    compose_rows(rows);
    check("clock before CLU_RTC_MIN_VALID: not set", strcmp(rows[4], "Time not set") == 0);
    check("uptime under a minute", strcmp(rows[6], "Up 0d 00:00:59") == 0);
}

// ---------------------------------------------------------------- clu_status_display.c task
static void test_status_task(void)
{
    s_now = 1735689600; // 2025-01-01 00:00:00 UTC
    s_draw_count = 0;
    s_delays_left = 2;
    if (setjmp(s_task_exit) == 0) {
        s_task_function(NULL);
    }
    check("task refreshes every second", s_last_delay_ticks == pdMS_TO_TICKS(1000));
    check("task shows the composed rows", screen_row_shows(0, "CLU 123456789") &&
                                              screen_row_shows(4, "2025-01-01 00:00:00"));
    check("second refresh sends nothing unchanged", s_draw_count > 0 && s_draw_count < 2 * CLU_DISPLAY_ROWS);
}

/** @brief The fake panel's page for @p row equals @p text rendered. */
static bool screen_row_shows(int row, const char *text)
{
    uint8_t expected_page[DISPLAY_TEXT_WIDTH];
    display_text_render_row(text, expected_page);
    return memcmp(s_screen_pages[row], expected_page, sizeof expected_page) == 0;
}

// ---------------------------------------------------------------- fakes: I2C bus
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *c, i2c_master_bus_handle_t *bus)
{
    if (s_fail_step == FAIL_BUS) {
        return ESP_FAIL;
    }
    s_fake_bus = (struct host_i2c_bus){c->sda_io_num, c->scl_io_num};
    *bus = &s_fake_bus;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout_ms)
{
    return addr == s_chip_address ? ESP_OK : ESP_ERR_TIMEOUT;
}

// ---------------------------------------------------------------- fakes: esp_lcd
esp_err_t esp_lcd_new_panel_io_i2c(i2c_master_bus_handle_t bus, const esp_lcd_panel_io_i2c_config_t *io_config,
                                   esp_lcd_panel_io_handle_t *ret_io)
{
    s_last_io_config = *io_config;
    if (s_fail_step == FAIL_PANEL_IO) {
        return ESP_FAIL;
    }
    *ret_io = &s_fake_panel_io;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io)
{
    s_panel_io_deletes++;
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_ssd1306(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    s_last_panel_config = *panel_dev_config;
    s_last_ssd1306_config = *(const esp_lcd_panel_ssd1306_config_t *)panel_dev_config->vendor_config;
    if (s_fail_step == FAIL_SSD1306) {
        return ESP_FAIL;
    }
    s_panel_creates++;
    *ret_panel = &s_fake_panel;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t panel)
{
    return s_fail_step == FAIL_RESET ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t panel)
{
    return s_fail_step == FAIL_INIT ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t panel, bool mirror_x, bool mirror_y)
{
    s_mirror_x = mirror_x;
    s_mirror_y = mirror_y;
    return s_fail_step == FAIL_MIRROR ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on_off)
{
    if (s_fail_step == FAIL_DISPLAY_ON) {
        return ESP_FAIL;
    }
    s_display_on = on_off;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t panel)
{
    s_panel_deletes++;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start, int x_end, int y_end,
                                    const void *color_data)
{
    s_draw_count++;
    s_last_draw_x_start = x_start;
    s_last_draw_y_start = y_start;
    s_last_draw_x_end = x_end;
    s_last_draw_y_end = y_end;
    if (s_draw_fails) {
        return ESP_FAIL;
    }
    memcpy(s_screen_pages[y_start / 8], color_data, DISPLAY_TEXT_WIDTH);
    return ESP_OK;
}

// ---------------------------------------------------------------- fakes: FreeRTOS
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *h)
{
    s_task_creates++;
    s_task_function = fn;
    s_task_name = name;
    s_task_stack_size = stack;
    s_task_priority = prio;
    return pdTRUE;
}

void vTaskDelay(TickType_t t)
{
    s_last_delay_ticks = t;
    if (--s_delays_left <= 0) {
        longjmp(s_task_exit, 1);
    }
}

// ---------------------------------------------------------------- fakes: network, clock, bus
uint32_t clu_net_ip(clu_net_if_t interface_index)
{
    return s_interface_ips[interface_index];
}

char *esp_ip4addr_ntoa(const esp_ip4_addr_t *a, char *buf, int len)
{
    snprintf(buf, len, IPSTR, IP2STR(a));
    return buf;
}

static time_t host_time(time_t *timer)
{
    if (timer) {
        *timer = s_now;
    }
    return s_now;
}

int64_t esp_timer_get_time(void)
{
    return s_uptime_us;
}

int tfbus_master_count(void)
{
    return (int)(sizeof s_module_slots / sizeof s_module_slots[0]);
}

tfbus_module_t *tfbus_master_get(int module_index)
{
    return s_module_slots[module_index];
}
