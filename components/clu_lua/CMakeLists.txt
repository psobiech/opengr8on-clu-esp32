idf_component_register(
    SRCS "clu_lua.c"
    INCLUDE_DIRS "include"
    REQUIRES lua esp_timer heap
    EMBED_TXTFILES "prelude.lua" "objects.lua"
)
