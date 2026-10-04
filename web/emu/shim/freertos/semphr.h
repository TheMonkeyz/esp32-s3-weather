#pragma once
#include "freertos/FreeRTOS.h"
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return (SemaphoreHandle_t)1; }
#define xSemaphoreTake(s, t) pdTRUE
#define xSemaphoreGive(s) pdTRUE
#define xSemaphoreTakeRecursive(s, t) pdTRUE
#define xSemaphoreGiveRecursive(s) pdTRUE
