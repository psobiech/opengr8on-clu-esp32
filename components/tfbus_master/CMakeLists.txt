idf_component_register(
    SRCS "tfbus_master.c"
    INCLUDE_DIRS "include"
    REQUIRES clu_proto tfbus_uart clu_lua nvs_flash
)
