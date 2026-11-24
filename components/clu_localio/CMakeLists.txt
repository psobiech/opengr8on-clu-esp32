idf_component_register(
    SRCS "localio.c" "clu_localio.c" "localio_sensors.c" "clu_localio_sensors.c"
    INCLUDE_DIRS "include"
    REQUIRES clu_proto
    PRIV_REQUIRES tfbus_uart clu_crypto clu_i2c esp_driver_i2c esp_driver_gpio esp_adc esp_timer nvs_flash log freertos
)
