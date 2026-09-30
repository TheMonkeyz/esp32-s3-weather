#pragma once
#include <stdbool.h>
#include "lvgl.h"

lv_obj_t *radar_create(lv_font_t *f_title, lv_font_t *f_small, lv_font_t *f_micro);
void radar_set_visible(bool visible);
void radar_relocate(void);   // location changed: new map + radar
void radar_zoom(int step);   // +1 zoom in, -1 zoom out (call from the LVGL task)

// Download every zoom level's map that isn't cached yet, in the background.
// While it runs, the radar screen shows a "Preparing maps" panel.
void radar_preload_start(void);
