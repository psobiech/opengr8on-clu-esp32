// Host stub: the part of ESP-IDF's panic_info_t that main/clu_crash.c reads.
#pragma once
#include <stdbool.h>
typedef enum {
    PANIC_EXCEPTION_DEBUG, PANIC_EXCEPTION_IWDT, PANIC_EXCEPTION_TWDT, PANIC_EXCEPTION_ABORT, PANIC_EXCEPTION_FAULT
} panic_exception_t;
typedef struct {
    int core;
    panic_exception_t exception;
    const char *reason;
    const char *description;
    const void *addr;
} panic_info_t;
extern bool g_panic_abort;          // defined by the test
extern char *g_panic_abort_details; // defined by the test
