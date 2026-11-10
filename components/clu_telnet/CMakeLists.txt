idf_component_register(
    SRCS "clu_telnet.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip log
    PRIV_REQUIRES heap
)
