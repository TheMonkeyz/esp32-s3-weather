#pragma once
#include <stdbool.h>
#include "lvgl.h"

lv_obj_t *radar_create(lv_font_t *f_title, lv_font_t *f_small, lv_font_t *f_micro);
void radar_set_visible(bool visible);
void radar_relocate(void);   // location changed: new map + radar
