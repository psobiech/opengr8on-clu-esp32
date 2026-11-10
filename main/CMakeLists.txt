idf_component_register(
    SRCS "main.c" "clu_state.c" "udp_server.c" "clu_time.c" "clu_net.c" "clu_diag.c" "clu_sysinfo.c"
    INCLUDE_DIRS "."
    REQUIRES clu_proto clu_crypto clu_tftp clu_lua clu_mqtt clu_telnet tfbus_uart tfbus_master tfbus_drivers
             nvs_flash esp_wifi esp_netif esp_event fatfs wear_levelling lwip esp_system esp_timer esp_driver_gpio
)
