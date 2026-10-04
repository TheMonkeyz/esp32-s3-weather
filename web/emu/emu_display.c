// The panel, in the browser: a 466x466 RGB565 framebuffer the page copies to its canvas (emu.js, on every animation
// frame when emu_fb_dirty() says so). Implements main/display.h: LVGL's flush and the raw frames of slide.c.
#include <string.h>
#include <emscripten.h>
#include "display.h"

static uint16_t fb[DISP_W * DISP_H];       // RGB565 as LVGL draws it
static int dirty = 1;
static display_flush_hook_t flush_hook;
static uint8_t level = 255;

EMSCRIPTEN_KEEPALIVE uint16_t *emu_fb(void) { return fb; }
EMSCRIPTEN_KEEPALIVE int emu_fb_dirty(void) { int d = dirty; dirty = 0; return d; }
EMSCRIPTEN_KEEPALIVE int emu_brightness(void) { return level; }

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    if (flush_hook) flush_hook(a, px);
    int w = a->x2 - a->x1 + 1;
    const uint16_t *src = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++) {
        if (y < 0 || y >= DISP_H) { src += w; continue; }
        memcpy(&fb[y * DISP_W + a->x1], src, w * 2);
        src += w;
    }
    dirty = 1;
    lv_display_flush_ready(disp);
}

void display_init(void)
{
    static uint16_t b1[DISP_W * 32], b2[DISP_W * 32];      // 32-row bands, as the display's
    lv_display_t *d = lv_display_create(DISP_W, DISP_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, b1, b2, sizeof(b1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
}

bool display_lock(int timeout_ms) { (void)timeout_ms; return true; }   // one thread
void display_unlock(void) {}
void display_brightness(uint8_t l) { level = l; }
void display_get_stats(display_stats_t *out, bool reset) { (void)reset; memset(out, 0, sizeof(*out)); }
void display_get_test_stats(display_stats_t *out, bool reset) { (void)reset; memset(out, 0, sizeof(*out)); }
void display_bench_no_panel(bool on) { (void)on; }
void display_set_flush_hook(display_flush_hook_t hook) { flush_hook = hook; }

static inline uint16_t unswap(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }

// slide.c's frames are in the panel's byte order (swapped): back to LVGL's, row band by row band
void display_raw_frame(display_fill_cb_t fill, void *user)
{
    static uint16_t band[DISP_W * 32];
    for (int y0 = 0; y0 < DISP_H; y0 += 32) {
        int n = y0 + 32 > DISP_H ? DISP_H - y0 : 32;
        fill(y0, n, band, user);
        for (int i = 0; i < DISP_W * n; i++) fb[y0 * DISP_W + i] = unswap(band[i]);
    }
    dirty = 1;
    emscripten_sleep(0);                                   // let the page show it (the browser paints between frames)
}

void display_raw_area(int x0, int y0, int x1, int y1, bool bottom_up, display_area_fill_cb_t fill, void *user)
{
    static uint16_t band[DISP_W * 32];
    if (x0 & 1) x0--;                                      // even/odd edges, as the panel needs
    if (!(x1 & 1)) x1++;
    if (x1 > DISP_W - 1) x1 = DISP_W - 1;
    int w = x1 - x0 + 1, rows = (DISP_W * 32 / w) & ~1;
    if (rows < 2) rows = 2;
    int h = y1 - y0 + 1;
    for (int k = 0; k < h; k += rows) {
        int n = k + rows > h ? h - k : rows;
        int y = bottom_up ? y1 - k - n + 1 : y0 + k;
        fill(x0, w, y, n, band, user);
        for (int r = 0; r < n; r++)
            for (int c = 0; c < w; c++) fb[(y + r) * DISP_W + x0 + c] = unswap(band[r * w + c]);
    }
    dirty = 1;
    emscripten_sleep(0);
}
