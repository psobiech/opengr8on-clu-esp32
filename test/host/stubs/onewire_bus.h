// Host stub: the espressif/onewire_bus API used by clu_localio_sensors.c (the test implements it).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct onewire_bus_t *onewire_bus_handle_t;
typedef uint64_t onewire_device_address_t;
typedef struct {
    int bus_gpio_num;
    struct {
        uint32_t en_pull_up : 1;
    } flags;
} onewire_bus_config_t;
typedef struct {
    uint32_t max_rx_bytes;
} onewire_bus_rmt_config_t;
esp_err_t onewire_new_bus_rmt(const onewire_bus_config_t *bus_config, const onewire_bus_rmt_config_t *rmt_config,
                              onewire_bus_handle_t *ret_bus);
esp_err_t onewire_bus_reset(onewire_bus_handle_t bus);
esp_err_t onewire_bus_write_bytes(onewire_bus_handle_t bus, const uint8_t *tx_data, uint8_t tx_data_size);
esp_err_t onewire_bus_read_bytes(onewire_bus_handle_t bus, uint8_t *rx_buf, size_t rx_buf_size);
