idf_component_register(
    SRCS "clu_mysensors.c" "ms_vmod.c"
    INCLUDE_DIRS "include"
    REQUIRES clu_proto clu_lua lua tfbus_uart
    PRIV_REQUIRES esp_driver_uart esp_timer nvs_flash esp_hw_support
    EMBED_TXTFILES "mysensors.lua"
)
