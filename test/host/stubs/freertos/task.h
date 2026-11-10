#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void xTaskNotifyGive(TaskHandle_t t);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *h, BaseType_t core);
// Tests that need time to pass advance host_ticks themselves.
static TickType_t host_ticks;
static inline TickType_t xTaskGetTickCount(void) { return host_ticks; }
void vTaskDelete(TaskHandle_t t);
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *h);
void vTaskSuspend(TaskHandle_t t);
