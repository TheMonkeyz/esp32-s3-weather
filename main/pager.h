#pragma once
#include <stdbool.h>
#include "lvgl.h"

// Full-screen pager: pages side by side (horizontal) or stacked (vertical) in a scroller that snaps one page at a
// time, so a page follows the finger, snaps into place, and bounces at the first and last page (LVGL elastic
// scrolling). Used by the hourly view (days) and the weather screen (places).
//
// Pages are plain containers: not clickable, presses and gestures bubble up through the pager (which stays
// clickable so it can scroll) to the screen. A gesture across the pager's direction still reaches the screen.

typedef void (*pager_cb_t)(int page, void *user);

// on_change: while dragging, each time another page reaches the middle (page dots).
// on_settle: when scrolling stops, with the page it settled on.
lv_obj_t *pager_create(lv_obj_t *parent, bool vertical, int pages, pager_cb_t on_change, pager_cb_t on_settle,
                       void *user);
lv_obj_t *pager_page(lv_obj_t *pager, int i);
void pager_go(lv_obj_t *pager, int i, bool anim);   // show page i
int pager_current(lv_obj_t *pager);                // the page in the middle now
