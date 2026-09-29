#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#define DISP_W 466
#define DISP_H 466

void display_init(void);
bool display_lock(int timeout_ms);
void display_unlock(void);
void display_brightness(uint8_t level);
