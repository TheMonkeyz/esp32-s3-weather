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

// For other screens (alert map): the cached basemap for zoom z (4..10), W x H RGB565 centred on the location,
// and its top-left corner in world pixels at that zoom. False if that level isn't cached yet.
#define RADAR_ZOOM_MIN 4
#define RADAR_ZOOM_MAX 10
bool radar_basemap_read(int z, uint16_t *dst, double *ox, double *oy);
bool radar_osm_render(int z, double ox, double oy, uint16_t *dst, int w, int h);   // dimmed OSM tiles for any window
