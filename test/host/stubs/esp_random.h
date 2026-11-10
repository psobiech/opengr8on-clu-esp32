#pragma once
#include <stddef.h>
#include <sys/random.h>
static inline void esp_fill_random(void *buf, size_t len)
{
    if (getrandom(buf, len, 0) != (ssize_t)len) {
        __builtin_trap();
    }
}
#include <stdint.h>
static inline uint32_t esp_random(void)
{
    uint32_t r;
    esp_fill_random(&r, sizeof r);
    return r;
}
