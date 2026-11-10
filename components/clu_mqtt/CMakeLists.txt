idf_component_register(
    SRCS "clu_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES clu_lua lua
    PRIV_REQUIRES mqtt mbedtls
    EMBED_TXTFILES "mqtt.lua"
)
