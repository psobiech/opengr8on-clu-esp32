idf_component_register(
    SRCS "tfbus_drivers.c" "drv_digital_io.c" "drv_roller.c" "drv_touch_panel_fm4.c" "drv_smart_panel_fm4.c" "drv_panel_common.c" "drv_light.c" "drv_dimmer.c" "drv_ledrgb.c"
    INCLUDE_DIRS "include"
    REQUIRES tfbus_master clu_proto clu_lua lua esp_timer nvs_flash
)
