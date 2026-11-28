// Host stub: just enough FreeRTOS for tfbus_master.c (single-threaded test).
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS pdTRUE
#define portMAX_DELAY 0xffffffffu
#define configMAX_TASK_NAME_LEN 16
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portTICK_PERIOD_MS 1
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
