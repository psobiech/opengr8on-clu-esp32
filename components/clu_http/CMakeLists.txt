idf_component_register(
    SRCS "clu_http.c"
    INCLUDE_DIRS "include"
    REQUIRES clu_lua lua
    PRIV_REQUIRES clu_proto esp_http_client mbedtls lwip
    EMBED_TXTFILES "http.lua"
)
