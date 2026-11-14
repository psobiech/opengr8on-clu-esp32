/**
 * @file tfbus_drivers.c
 * @brief Driver registration and frame-queueing helpers shared by all module drivers.
 */
#include "tfbus_drivers.h"

#include "esp_log.h"
#include "tfbus_frame.h"

void drv_queue_record(tfbus_module_t *module, uint8_t type, uint8_t meta, const uint8_t *data, uint8_t len)
{
    uint8_t frame[TFBUS_MAX_FRAME];
    tfbus_tlv_t record = {.type = type, .meta = meta, .len = len, .data = data};
    size_t frame_length = tfbus_module_cmd(frame, sizeof frame, module->addr, &record, 1);
    if (frame_length == 0 || tfbus_master_queue(frame, frame_length) != ESP_OK) {
        ESP_LOGW("drv", "queue full / bad record for addr %u", module->addr);
    }
}

void drv_queue_records(tfbus_module_t *module, const tfbus_tlv_t *recs, size_t record_count)
{
    uint8_t frame[TFBUS_MAX_FRAME];
    size_t len = tfbus_module_cmd(frame, sizeof frame, module->addr, recs, record_count);
    if (len == 0 || tfbus_master_queue(frame, len) != ESP_OK) {
        ESP_LOGW("drv", "queue full / bad batch for addr %u", module->addr);
    }
}

void drv_queue_frame(tfbus_module_t *module, uint8_t type, const uint8_t *payload, size_t len)
{
    uint8_t frame[TFBUS_MAX_FRAME];
    size_t frame_length = tfbus_build(frame, sizeof frame, module->addr, type, payload, len);
    if (frame_length == 0 || tfbus_master_queue(frame, frame_length) != ESP_OK) {
        ESP_LOGW("drv", "queue full / bad frame for addr %u", module->addr);
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
    tfbus_driver_register(&tfbus_drv_dimmer_mosfet_din);
    tfbus_driver_register(&tfbus_drv_led_rgbw_din);
}
