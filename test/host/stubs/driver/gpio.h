// Host stub: the ESP-IDF GPIO driver calls used by udp_server.c, implemented by the test.
#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef int gpio_num_t;
typedef enum { GPIO_MODE_INPUT = 1 } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *c);
int gpio_get_level(gpio_num_t gpio);
