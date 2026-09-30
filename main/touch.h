#pragma once
#include "lvgl.h"
#include "driver/i2c_master.h"

void touch_init(void);          // I2C + controller reset
void touch_register_lvgl(void); // call with the display lock held
i2c_master_bus_handle_t touch_i2c_bus(void);   // shared with the audio codec
