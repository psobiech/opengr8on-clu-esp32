// Host stub: just enough FreeRTOS for tfbus_master.c (single-threaded test).
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
