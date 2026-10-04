#pragma once
#include "freertos/FreeRTOS.h"
static inline void vTaskDelay(TickType_t t) { emscripten_sleep(t ? t : 1); }
static inline TickType_t xTaskGetTickCount(void) { return (TickType_t)emscripten_get_now(); }
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)1; }
static inline BaseType_t xTaskNotifyGive(TaskHandle_t t) { (void)t; return pdPASS; }
static inline uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t) { (void)c; emscripten_sleep(t > 50 ? 50 : t); return 0; }
static inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 4096; }
