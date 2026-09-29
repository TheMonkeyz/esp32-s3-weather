#pragma once
#include "lvgl.h"

void touch_init(void);          // I2C + controller reset
void touch_register_lvgl(void); // call with the display lock held
