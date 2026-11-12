idf_component_register(
    SRCS "clu_rtc.c" "pcf85063.c" "ds3231.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES clu_i2c esp_driver_i2c log
)
