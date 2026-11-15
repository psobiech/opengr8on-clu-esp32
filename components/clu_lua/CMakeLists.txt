idf_component_register(
    SRCS "clu_lua.c"
    INCLUDE_DIRS "include"
    REQUIRES lua esp_timer heap
)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    clu_lua_embed(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/prelude.lua")
    clu_lua_embed(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/objects.lua")
endif()
