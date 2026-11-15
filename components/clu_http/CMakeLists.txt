idf_component_register(
    SRCS "clu_http.c"
    INCLUDE_DIRS "include"
    REQUIRES clu_lua lua
    PRIV_REQUIRES clu_proto esp_http_client mbedtls lwip
)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    clu_lua_embed(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/http.lua")
endif()
