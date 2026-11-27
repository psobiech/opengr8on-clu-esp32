// Host stub: chip identification (esp_hw_support).
#pragma once
#include <stdint.h>
typedef struct {
    uint16_t revision; /**< major * 100 + minor */
} esp_chip_info_t;
void esp_chip_info(esp_chip_info_t *out_info);
