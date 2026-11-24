// Host stub: flash chip queries (spi_flash). The default chip reports 16 MB.
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_flash_t esp_flash_t;
static inline esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size)
{
    (void)chip;
    *out_size = 16u << 20;
    return ESP_OK;
}
