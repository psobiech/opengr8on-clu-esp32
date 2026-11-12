idf_component_register(
    SRCS "clu_rtc.c" "pcf85063.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2c log
)
