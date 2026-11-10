#pragma once
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (SemaphoreHandle_t)2; }
// Mutexes (handle 1) count how deep they are held, so a test can catch a missing give.
static int host_mutex_held;
// Set by a test to model another task holding the mutexes: try-locks (timeout 0) fail.
static int host_mutex_busy;
// Binary semaphores (handle 2) share one flag: given / not given. A take on an empty one returns
// pdFALSE at once (single-threaded: nobody else could give it while waiting).
static int host_binary_given;
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    if (s == (SemaphoreHandle_t)2) {
        BaseType_t got = host_binary_given ? pdTRUE : pdFALSE;
        host_binary_given = 0;
        return got;
    }
    if (host_mutex_busy && t == 0) {
        return pdFALSE;
    }
    host_mutex_held += s == (SemaphoreHandle_t)1;
    return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (s == (SemaphoreHandle_t)2) {
        host_binary_given = 1;
        return pdTRUE;
    }
    host_mutex_held -= s == (SemaphoreHandle_t)1;
    return pdTRUE;
}
