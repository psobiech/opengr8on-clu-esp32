idf_component_register(
    SRCS "display_text.c" "clu_display.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES clu_i2c esp_lcd esp_driver_i2c log
)
