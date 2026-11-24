// Host stub: the espressif/onewire_bus device search used by clu_localio_sensors.c (the test implements it).
#pragma once
#include "onewire_bus.h"
typedef struct onewire_device_iter_t *onewire_device_iter_handle_t;
typedef struct onewire_device_t {
    onewire_bus_handle_t bus;
    onewire_device_address_t address;
} onewire_device_t;
esp_err_t onewire_new_device_iter(onewire_bus_handle_t bus, onewire_device_iter_handle_t *ret_iter);
esp_err_t onewire_del_device_iter(onewire_device_iter_handle_t iter);
esp_err_t onewire_device_iter_get_next(onewire_device_iter_handle_t iter, onewire_device_t *dev);
