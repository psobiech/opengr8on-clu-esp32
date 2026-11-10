#pragma once
#include "FreeRTOS.h"
typedef struct host_event_group *EventGroupHandle_t;
typedef uint32_t EventBits_t;
#define BIT0 0x1u
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b);
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t b, BaseType_t clear, BaseType_t all, TickType_t t);
