#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
typedef int wl_handle_t;
typedef struct { bool format_if_mount_failed; int max_files; size_t allocation_unit_size; } esp_vfs_fat_mount_config_t;
esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *base, const char *label, const esp_vfs_fat_mount_config_t *c,
                                           wl_handle_t *wl);
