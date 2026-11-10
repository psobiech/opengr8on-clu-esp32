idf_component_register(
    SRCS "clu_crypto.c"
    INCLUDE_DIRS "include"
    REQUIRES clu_proto mbedtls esp_hw_support
)
