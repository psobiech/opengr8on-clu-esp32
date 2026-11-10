/**
 * @file tfbus_drivers.c
 * @brief Driver registration and frame-queueing helpers shared by all module drivers.
 */
#include "tfbus_drivers.h"

#include "esp_log.h"
#include "tfbus_frame.h"

void drv_queue_record(tfbus_module_t *m, uint8_t type, uint8_t meta, const uint8_t *data, uint8_t len)
{
    uint8_t f[TFBUS_MAX_FRAME];
    tfbus_tlv_t r = {.type = type, .meta = meta, .len = len, .data = data};
    size_t n = tfbus_module_cmd(f, sizeof f, m->addr, &r, 1);
    if (n == 0 || tfbus_master_queue(f, n) != ESP_OK) {
        ESP_LOGW("drv", "queue full / bad record for addr %u", m->addr);
    }
}

void drv_queue_records(tfbus_module_t *m, const tfbus_tlv_t *recs, size_t n)
{
    uint8_t f[TFBUS_MAX_FRAME];
    size_t len = tfbus_module_cmd(f, sizeof f, m->addr, recs, n);
    if (len == 0 || tfbus_master_queue(f, len) != ESP_OK) {
        ESP_LOGW("drv", "queue full / bad batch for addr %u", m->addr);
    }
}

void drv_queue_frame(tfbus_module_t *m, uint8_t type, const uint8_t *payload, size_t len)
{
    uint8_t f[TFBUS_MAX_FRAME];
    size_t n = tfbus_build(f, sizeof f, m->addr, type, payload, len);
    if (n == 0 || tfbus_master_queue(f, n) != ESP_OK) {
        ESP_LOGW("drv", "queue full / bad frame for addr %u", m->addr);
    }
}

void tfbus_drivers_register_all(void)
{
    tfbus_driver_register(&tfbus_drv_io_module_din8);
    tfbus_driver_register(&tfbus_drv_roller_sh_din3);
    tfbus_driver_register(&tfbus_drv_touch_panel_fm4);
    tfbus_driver_register(&tfbus_drv_relay_din4);
    tfbus_driver_register(&tfbus_drv_roller_sh_din);
    tfbus_driver_register(&tfbus_drv_smart_panel_fm4);
}
