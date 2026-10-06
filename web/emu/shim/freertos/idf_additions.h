#pragma once
// Browser emulator: ESP-IDF's FreeRTOS additions radar.c uses (where a task's stack lives doesn't matter here)
#include "freertos/task.h"
static inline BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                                         UBaseType_t prio, TaskHandle_t *out, BaseType_t core,
                                                         UBaseType_t caps)
{
    (void)caps;
    return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, core);
}
