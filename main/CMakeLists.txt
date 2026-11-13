idf_component_register(
    SRCS "main.c" "clu_state.c" "udp_server.c" "clu_time.c" "clu_net.c" "clu_diag.c" "clu_sysinfo.c" "clu_eth.c"
    INCLUDE_DIRS "."
    REQUIRES clu_proto clu_crypto clu_rtc clu_tftp clu_lua clu_mqtt clu_ota clu_telnet tfbus_uart tfbus_master tfbus_drivers clu_localio
             nvs_flash esp_wifi esp_eth esp_netif esp_event fatfs wear_levelling lwip esp_system esp_timer esp_driver_gpio esp_driver_spi
)
