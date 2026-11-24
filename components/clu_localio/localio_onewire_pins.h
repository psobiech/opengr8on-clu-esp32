/**
 * @file localio_onewire_pins.h
 * @brief The 1-Wire bus pins as one comma-separated list (LOCALIO_ONEWIRE_GPIOS): on the KinCony F8
 * the terminals set to 1-Wire per DIN (CLU_LOCAL_IO_F8_DINx_MODE), elsewhere
 * CLU_LOCAL_IO_ONEWIRE_GPIOS. Private to clu_localio (GPIO input module and sensor glue).
 */
#pragma once

#include <stdbool.h>
#include <stdlib.h>

#include "sdkconfig.h"

#if CONFIG_CLU_BOARD_KINCONY_F8
#if CONFIG_CLU_LOCAL_IO_F8_DIN1_ONEWIRE
#define LOCALIO_F8_ONEWIRE_DIN1 "48,"
#else
#define LOCALIO_F8_ONEWIRE_DIN1 ""
#endif
#if CONFIG_CLU_LOCAL_IO_F8_DIN2_ONEWIRE
#define LOCALIO_F8_ONEWIRE_DIN2 "47,"
#else
#define LOCALIO_F8_ONEWIRE_DIN2 ""
#endif
#if CONFIG_CLU_LOCAL_IO_F8_DIN3_ONEWIRE
#define LOCALIO_F8_ONEWIRE_DIN3 "21,"
#else
#define LOCALIO_F8_ONEWIRE_DIN3 ""
#endif
#if CONFIG_CLU_LOCAL_IO_F8_DIN4_ONEWIRE
#define LOCALIO_F8_ONEWIRE_DIN4 "15,"
#else
#define LOCALIO_F8_ONEWIRE_DIN4 ""
#endif
#define LOCALIO_ONEWIRE_GPIOS \
    LOCALIO_F8_ONEWIRE_DIN1 LOCALIO_F8_ONEWIRE_DIN2 LOCALIO_F8_ONEWIRE_DIN3 LOCALIO_F8_ONEWIRE_DIN4
#else
#define LOCALIO_ONEWIRE_GPIOS CONFIG_CLU_LOCAL_IO_ONEWIRE_GPIOS
#endif

/** @brief Max 1-Wire buses (each takes one RMT TX and one RX channel). */
#define LOCALIO_ONEWIRE_BUSES_MAX 4

/**
 * @brief Parse LOCALIO_ONEWIRE_GPIOS ("48,21") into @p pins (separators: commas, spaces).
 * @return Number of pins (at most @p capacity); parsing stops at the first non-number.
 */
static inline int localio_onewire_pins(int *pins, int capacity)
{
    int pin_count = 0;
    const char *cursor = LOCALIO_ONEWIRE_GPIOS;
    while (pin_count < capacity) {
        while (*cursor == ',' || *cursor == ' ') {
            cursor++;
        }
        char *after_number;
        long pin_number = strtol(cursor, &after_number, 10);
        if (after_number == cursor) {
            break;
        }
        pins[pin_count++] = (int)pin_number;
        cursor = after_number;
    }
    return pin_count;
}

/** @brief True if @p pin is a 1-Wire bus pin. */
static inline bool localio_is_onewire_pin(long pin)
{
    int pins[LOCALIO_ONEWIRE_BUSES_MAX];
    int pin_count = localio_onewire_pins(pins, LOCALIO_ONEWIRE_BUSES_MAX);
    for (int i = 0; i < pin_count; i++) {
        if (pins[i] == pin) {
            return true;
        }
    }
    return false;
}
