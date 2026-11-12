idf_component_register(
    SRCS "localio.c" "clu_localio.c"
    INCLUDE_DIRS "include"
    REQUIRES clu_proto
    PRIV_REQUIRES tfbus_uart clu_crypto clu_i2c esp_driver_i2c esp_timer nvs_flash log
)
