idf_component_register(
    SRCS "clu_i2c.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES log
)
