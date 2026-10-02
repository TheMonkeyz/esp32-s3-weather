#pragma once
#include "lvgl.h"
#include "driver/i2c_master.h"

void touch_init(void);          // I2C + controller reset
void touch_register_lvgl(void); // call with the display lock held
i2c_master_bus_handle_t touch_i2c_bus(void);   // shared with the audio codec
void touch_inject(bool down, int x, int y);    // test console: simulated finger (screen pixels)
void touch_inject_end(void);                   // back to the real touch controller
int touch_get(int *x, int *y);                 // finger now: 1 pressed, 0 up, -1 bus error (display lock held)
void touch_forget(void);                       // the touch LVGL last saw is over (handled by a drag, slide.c)
uint32_t touch_idle_ms(void);                  // ms since a finger was last down (any task)
