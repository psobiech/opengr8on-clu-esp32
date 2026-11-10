idf_component_register(
    SRCS "clu_tftp.c"
    INCLUDE_DIRS "include"
    REQUIRES clu_proto lwip heap
    PRIV_REQUIRES esp_timer
)
