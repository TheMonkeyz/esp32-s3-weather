#pragma once
#include <stdbool.h>
#include "lvgl.h"

lv_obj_t *radar_create(lv_font_t *f_title, lv_font_t *f_small, lv_font_t *f_micro);
void radar_set_visible(bool visible);
void radar_relocate(void);   // location changed: new map + radar
void radar_zoom(int step);   // +1 zoom in, -1 zoom out (call from the LVGL task)
void radar_units_changed(void);   // clock format / distance unit changed: redraw the labels

// Download every zoom level's map that isn't cached yet, in the background.
// While it runs, the radar screen shows a "Preparing maps" panel.
void radar_preload_start(void);

// For other screens (alert map): a w x h RGB565 window at (x0, y0) of the cached basemap for zoom z (4..10), the map
// whose top-left corner is (ox, oy) in world pixels at that zoom (W x H, centred on a location). False if that level
// isn't cached for that map.
#define RADAR_ZOOM_MIN 4
#define RADAR_ZOOM_MAX 10
bool radar_basemap_crop(int z, double ox, double oy, int x0, int y0, int w, int h, uint16_t *dst);
bool radar_osm_render(int z, double ox, double oy, uint16_t *dst, int w, int h);   // dimmed OSM tiles for any window
