idf_component_register(
    SRCS "clu_ota.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server app_update esp_app_format esp_system esp_timer log freertos
)
