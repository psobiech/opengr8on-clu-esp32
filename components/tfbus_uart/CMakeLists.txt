idf_component_register(
    SRCS "tfbus_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES clu_proto esp_driver_uart esp_timer
)
