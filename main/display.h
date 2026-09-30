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
    uint64_t pixels;                               // pixels sent to the panel
    uint32_t lvgl_wait_max_us;                     // longest wait of the LVGL task for the lock
    uint32_t hold_max_us;                          // longest lock hold by another task...
    char hold_task[16];                            // ...and which task
} display_stats_t;
void display_get_stats(display_stats_t *out, bool reset);
void display_bench_no_panel(bool on);             // diag bench only
