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

// Diagnostics (accumulated since the last reset)
typedef struct {
    uint32_t frames, render_us, render_max_us;     // rendered frames and their render time
    uint32_t anim_frames, anim_us, anim_gap_max_us; // back-to-back frames (animations): interval sum / worst gap
    uint32_t anim_gap_max_at_ms;                   // ...when it ended (ms since the reset; test console only)
    char anim_gap_max_kind[12];                    // ...between which frames: "lvgl>move", "move>move", ...
    uint64_t pixels;                               // pixels sent to the panel
    uint32_t lvgl_wait_max_us;                     // longest wait of the LVGL task for the lock
    uint32_t hold_max_us;                          // longest lock hold by another task...
    char hold_task[16];                            // ...and which task
} display_stats_t;
void display_get_stats(display_stats_t *out, bool reset);        // diag.c
void display_get_test_stats(display_stats_t *out, bool reset);   // the test console's "fps" (own counters)
void display_bench_no_panel(bool on);             // diag bench only

// A frame drawn without LVGL (slide.c): fill() writes rows y0..y0+n-1, full width, RGB565 *byte-swapped* (panel
// order), into dst; display_raw_frame() sends each band while the next one is filled. Display lock held; afterwards
// LVGL must redraw before it flushes again (lv_obj_invalidate).
typedef void (*display_fill_cb_t)(int y0, int n, void *dst, void *user);
void display_raw_frame(display_fill_cb_t fill, void *user);
// Only the area x0..x1, y0..y1 (widened to even/odd edges as the CO5300 needs): fill() writes n rows of w pixels
// starting at column x0 (dst: w * n pixels, panel order). Narrower than the screen: fewer bytes, faster (list scrolls).
// bottom_up: the bands from the bottom one up (a fill that moves rows down in its source, in place).
typedef void (*display_area_fill_cb_t)(int x0, int w, int y0, int n, void *dst, void *user);
void display_raw_area(int x0, int y0, int x1, int y1, bool bottom_up, display_area_fill_cb_t fill, void *user);

// Called with every area LVGL sends to the panel, before the byte swap (RGB565 as LVGL draws it, w x h pixels), in the
// LVGL task: slide.c copies it into its picture of the screen shown, which then always matches the panel.
typedef void (*display_flush_hook_t)(const lv_area_t *a, const uint8_t *px);
void display_set_flush_hook(display_flush_hook_t hook);
