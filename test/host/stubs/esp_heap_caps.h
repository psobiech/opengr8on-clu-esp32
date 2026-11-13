// Host stub: capability-aware allocation maps to the host heap. host_heap_fail makes the next
// heap_caps_malloc/realloc calls fail (counted down), host_psram_free is what SPIRAM reports.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
static int host_heap_fail;
static size_t host_psram_free = 8u << 20;
static inline void *heap_caps_malloc(size_t n, uint32_t caps)
{
    (void)caps;
    if (host_heap_fail > 0 && --host_heap_fail >= 0) {
        return NULL;
    }
    return malloc(n);
}
static inline void *heap_caps_realloc(void *p, size_t n, uint32_t caps)
{
    (void)caps;
    if (host_heap_fail > 0 && --host_heap_fail >= 0) {
        return NULL;
    }
    return realloc(p, n);
}
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(uint32_t caps)
{
    return caps & MALLOC_CAP_SPIRAM ? host_psram_free : 200000;
}
static inline size_t heap_caps_get_largest_free_block(uint32_t caps) { return heap_caps_get_free_size(caps); }
static inline size_t heap_caps_get_total_size(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? 8u << 20 : 300000; }
