idf_component_register(
    SRCS "clu_noise.c" "clu_sec_engine.c" "clu_sec.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls
    PRIV_REQUIRES esp_timer esp_hw_support nvs_flash lwip
)
