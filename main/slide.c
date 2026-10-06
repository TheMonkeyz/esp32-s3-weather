// Screen changes and drags as sliding pictures (see slide.h)
#include "slide.h"
#include <math.h>
#include <stdlib.h>
#include "display.h"
#include "touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "i18n.h"                         // LANG_IU: how far a scrolled list's text reaches
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_private.h"                   // layers, the redraw list (inv_p), wait_until_release
#include "indev/lv_indev_private.h"         // wait_until_release: cleared after a drag (touch_resync)

static const char *TAG = "slide";
volatile int slide_phase;               // breadcrumbs for the test console's "where"
static bool req_pending(void);          // a slide or drag queued or running

/* ---------- frames ---------- */

typedef struct {
    const uint8_t *cur, *prev, *next;   // pictures (RGB565, PSRAM): prev = left / above, next = right / below; NULL = black
    uint32_t stride;                    // bytes per picture row
    bool vertical;
    int off;                            // where the current picture is: 0 = in place, < 0 moved left / up (next shows),
                                        // > 0 moved right / down (prev shows); even
} frame_t;

// n pixels, little-endian RGB565 -> panel byte order, two at a time (rows and offsets are even, so 4-byte aligned)
static inline void copy_swap(void *dst, const void *src, int n)
{
    uint32_t *d = dst;
    if (!src) { for (int i = 0; i < n / 2; i++) d[i] = 0; return; }
    const uint32_t *s = src;
    for (int i = 0; i < n / 2; i++) {
        uint32_t v = s[i];
        d[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    }
}

static inline const uint8_t *row(const uint8_t *pic, int y, uint32_t stride) { return pic ? pic + y * stride : NULL; }
static inline const uint8_t *at(const uint8_t *r, int x) { return r ? r + x * 2 : NULL; }

static void fill(int y0, int n, void *dst, void *user)
{
    const frame_t *f = user;
    uint8_t *d = dst;
    const int W = DISP_W, H = DISP_H, o = f->off;
    for (int y = y0; y < y0 + n; y++, d += W * 2) {
        if (!f->vertical) {                              // a row = the end of one picture + the start of the other
            const uint8_t *c = row(f->cur, y, f->stride);
            if (o <= 0) {
                int a = -o;
                copy_swap(d, at(c, a), W - a);
                copy_swap(d + (W - a) * 2, row(f->next, y, f->stride), a);
            } else {
                copy_swap(d, at(row(f->prev, y, f->stride), W - o), o);
                copy_swap(d + o * 2, c, W - o);
            }
        } else if (o <= 0) {                             // a row of one picture or the other
            int sy = y - o;
            copy_swap(d, sy < H ? row(f->cur, sy, f->stride) : row(f->next, sy - H, f->stride), W);
        } else {
            copy_swap(d, y < o ? row(f->prev, H - o + y, f->stride) : row(f->cur, y - o, f->stride), W);
        }
    }
}

static void show(frame_t *f, int off)
{
    f->off = off & ~1;
    display_raw_frame(fill, f);
}

// From the current offset to `to`, ease out, frames as fast as they go
static int animate(frame_t *f, int to, int ms)
{
    int from = f->off, frames = 0;
    int64_t start = esp_timer_get_time();
    for (;;) {
        int64_t el = esp_timer_get_time() - start;
        float t = el >= ms * 1000LL ? 1.0f : (float)el / (ms * 1000.0f);
        float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        show(f, from + (int)((to - from) * e));
        frames++;
        if (t >= 1.0f) return frames;
    }
}

/* While a slide or drag runs, LVGL is paused and never sees the finger lift. The touch that started it was told to
 * wait for its release (lv_indev_wait_release), so LVGL kept ignoring the next touch too, taking it for the same one:
 * quick successive swipes were missed. After a drag (its finger did lift) or a slide whose finger is up: LVGL starts
 * afresh, and a finger already down counts as a new press. */
static void touch_resync(bool lifted)
{
    int x, y;
    lv_indev_t *in = lv_indev_get_next(NULL);
    if (!in || (!lifted && touch_get(&x, &y) != 0)) return;
    touch_forget();                                      // and touch.c's memory of it (no fake press, then a tap)
    in->wait_until_release = 0;
    lv_indev_reset(in, NULL);
}

/* The finger, read directly, for the loops below (LVGL is paused, the display lock held). The CST9217 often stops
 * answering (NACK) when nothing touches it instead of reporting a release: 5 failed reads in a row after the finger was
 * last seen down count as a release, as in touch.c, and further failures stay "up" until a press. A loop that waited
 * for a clean "up" kept the display lock forever (the PSRAM-busy drag), or took a new swipe for the old one (the zoom).
 * *errs carries the state between calls: start it at 0 (finger down) or FINGER_UP.
 * 1 = down at x,y; 0 = up; -1 = a read error while down, not yet a release (keep the last point); -2 = the chip says
 * "up" but not for UP_HOLD_US yet (keep the last point; the list scroll starts coasting meanwhile). */
#define FINGER_UP 5
static int64_t last_touch;                 // the finger last seen by these loops (slide_last_touch)

static int err_lifts;                      // releases that were 5 read errors in a row, not a reported "up"
// A reported "up" counts once it has lasted UP_HOLD_US: during quick back-and-forth moves the CST9217 reports brief
// "ups" with the finger still down (37 in 1.3 s once, at the top of the hours list). Each one ended the scroll: the
// list coasted or sprang back, then took the finger for a new touch, and stopped under it.
#define UP_HOLD_US 60000
static int64_t up_since;                   // when a reported "up" began (0: the finger was seen down since)
static int brief_ups;                      // "ups" shorter than UP_HOLD_US, bridged
static int64_t brief_max;                  // the longest of them (us)

static int finger(int *x, int *y, int *errs)
{
    int r = touch_get(x, y);
    // The chip is read every 10 ms (touch_get): in between, the same answer, nothing counted again
    if (!touch_fresh()) return r > 0 ? 1 : *errs >= FINGER_UP ? 0 : up_since ? -2 : -1;
    int64_t now = esp_timer_get_time();
    if (r > 0) {
        if (up_since) {
            if (now - up_since > brief_max) brief_max = now - up_since;
            brief_ups++;
            up_since = 0;
        }
        *errs = 0;
        last_touch = now;
        return 1;
    }
    if (*errs >= FINGER_UP) return 0;              // up already
    if (r < 0 && !up_since) {                      // a read error: keep the last point, up to 5 in a row
        if (++*errs < FINGER_UP) return -1;
        *errs = FINGER_UP - 1;
        err_lifts++;                               // 5: the chip's silence, as good as an "up" (but held too:
    }                                              // they came in bursts while the finger moved, v1.12.1-fix.24)
    if (!up_since) up_since = now;
    if (now - up_since < UP_HOLD_US) return -2;    // maybe a brief "up": keep the last point
    up_since = 0;
    *errs = FINGER_UP;
    return 0;
}

// Safety caps for the loops that hold the display lock while a finger is down: a pure wait for the lift (3 s), and a
// loop that follows the finger (20 s: a slow list scroll while reading is legitimate; this only guards against a chip
// stuck reporting a press). After a cap the move ends as if the finger had lifted and LVGL takes the touch over.
#define FINGER_WAIT_US   3000000LL
#define FINGER_FOLLOW_US 20000000LL

/* ---------- pictures ---------- */

// 434 KB each in PSRAM (LVGL's heap), leaving ~1 MB for the rest: radar frames (217 KB each as they arrive), TLS, the
// hourly graphs. Set when the radar still decoded with lodepng (~870 KB blocks; with no margin free PSRAM fell to
// ~200 KB and radar decodes failed); png_rows.c needs ~50 KB, but the margin stays.
static bool room_for(int pictures)
{
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > (1000 + 440 * pictures) * 1024 &&
           heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 900 * 1024;
}

int64_t slide_last_touch(void) { return last_touch; }

bool slide_picture(lv_obj_t *scr, lv_draw_buf_t *dst) { return slide_picture_rows(scr, dst, 0, DISP_H - 1); }

// Rows y0..y1 of a screen into dst (a whole-screen RGB565 buffer): lv_snapshot_take_to_draw_buf() clipped to those
// rows, so a picture can be rendered a strip at a time (a whole one blocks LVGL for 60-180 ms: a quick swipe that
// started and ended meanwhile was never seen).
// LVGL skips a label whose box misses the rows drawn (lv_draw_label returns unless the clip meets the box itself), but
// Inuktitut's syllabics (the fallback font, drawn 5/4 larger from Montserrat's baseline) reach above and below the box:
// rows y0..y1 alone lost their faint tips (pictest after a Settings scroll in Inuktitut). From the fonts' metrics, Noto's
// tallest syllabic rises ROW_UP rows above the box at 28 px (7 at 20 px, 4 at 15) and its deepest goes ROW_DOWN below;
// Montserrat's own glyphs at most LATIN_UP / LATIN_DOWN (Å, Ǻ). So up + down more rows on each side are drawn too, as far
// as dst has room (rows top..top + h - 1), and rows y0 - up .. y1 + down come out right (see scroll_move_fill).
#define ROW_UP 9
#define ROW_DOWN 3
#define ROW_MARGIN (ROW_UP + ROW_DOWN)
#define LATIN_UP 4                        // Montserrat alone (no syllabics: lists in English or French), all its glyphs
#define LATIN_DOWN 1
static bool render_rows_reach(lv_obj_t *scr, lv_draw_buf_t *dst, int top, int y0, int y1, int up, int down)
{
    lv_obj_update_layout(scr);
    int last = top + (int)dst->header.h - 1;
    // Changed in dst: rows y0 - up .. y1 + down only, the rows every label reaching them was drawn for. The outer margin
    // rows are put back as they were: a label just past them was skipped, and a whole-screen dst had that label's
    // descender or top right (they were written as drawn until October 6)
    int g0 = y0 - up, g1 = y1 + down, nk = 0;
    y0 = LV_MAX(LV_MAX(y0 - up - down, top), 0);
    y1 = LV_MIN(LV_MIN(y1 + up + down, last), DISP_H - 1);
    static EXT_RAM_BSS_ATTR uint8_t keep[ROW_MARGIN][DISP_W * 2];
    uint32_t st = dst->header.stride;
    for (int y = y0; y <= y1; y++) if (y < g0 || y > g1) memcpy(keep[nk++], dst->data + (y - top) * st, DISP_W * 2);
    lv_area_t rows = {0, y0, DISP_W - 1, y1}, in_buf = {0, y0 - top, DISP_W - 1, y1 - top};
    lv_draw_buf_clear(dst, &in_buf);
    lv_layer_t layer;
    lv_memzero(&layer, sizeof(layer));
    layer.draw_buf = dst;
    layer.buf_area = (lv_area_t){0, top, DISP_W - 1, top + dst->header.h - 1};
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer._clip_area = rows;
    layer.phy_clip_area = rows;
    lv_display_t *d = lv_obj_get_display(scr), *old = lv_refr_get_disp_refreshing();
    lv_layer_t *old_head = d->layer_head;
    d->layer_head = &layer;
    lv_refr_set_disp_refreshing(d);
    lv_obj_redraw(&layer, scr);
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }
    d->layer_head = old_head;
    lv_refr_set_disp_refreshing(old);
    nk = 0;
    for (int y = y0; y <= y1; y++) if (y < g0 || y > g1) memcpy(dst->data + (y - top) * st, keep[nk++], DISP_W * 2);
    return true;
}

// Whole screens: syllabics may be anywhere (place and network names), in any language
static bool render_rows(lv_obj_t *scr, lv_draw_buf_t *dst, int top, int y0, int y1)
{
    return render_rows_reach(scr, dst, top, y0, y1, ROW_UP, ROW_DOWN);
}

bool slide_picture_rows(lv_obj_t *scr, lv_draw_buf_t *dst, int y0, int y1) { return render_rows(scr, dst, 0, y0, y1); }

/* ---------- picture cache ----------
 * Rendering a picture takes 50-130 ms: done at the start of a drag, the screen lagged the finger by ~0.1 s. So the
 * pictures a drag will need (the current screen and its neighbours) are kept ready: rendered while nothing happens
 * (slide_cache_idle_work), marked dirty when their screen changes. Five slots with fixed buffers, allocated as needed
 * (the weather screen: itself, extras, radar, the places above and below: up to 2.2 MB), freed when the radar decodes. */

#define CACHE_N 5
#define STRIP 64                         // rows per background step (~15-25 ms)
static struct {
    const void *key;
    lv_draw_buf_t *buf;
    bool dirty;                          // out of date, or not finished...
    int d0, d1;                          // ...in rows d0..d1 (the minute tick: only a clock's rows)
    int64_t dirty_since;
    int rows;                            // background rendering: the next row to render (d0 = not started)
} cache[CACHE_N];
static const void *keep[CACHE_N];
static int keep_n;
static slide_paint_cb_t paint_cb;
static slide_key_cb_t key_cb;
static bool painting;                    // a page's picture: its pager moves there and back, not a change
static bool committing;                  // a drag's switch: the pager scrolls to the new page, no content changes
static int64_t last_change;              // last invalidation of the screen shown
static const void *inv_key;              // the screen shown when LVGL was last asked to redraw...
static bool inv_pending;                 // ...and that redraw isn't on the panel yet

// Painting a page moves its pager there and back: those invalidations are not a change of the screen, and LVGL must
// not repaint for them (the panel still shows the same thing): its redraw list is put back as it was
static bool paint_rows(int e, int y0, int y1)
{
    lv_display_t *d = lv_display_get_default();
    uint32_t inv = d->inv_p;
    painting = true;
    bool ok = paint_cb(cache[e].key, cache[e].buf, y0, y1);
    painting = false;
    d->inv_p = inv;
    return ok;
}

static int find(const void *key)
{
    for (int i = 0; i < CACHE_N; i++) if (key && cache[i].key == key) return i;
    return -1;
}

// How much a slot is worth keeping: its place in keep[] (0 = what's shown), CACHE_N if not needed now
static int priority(int e)
{
    for (int i = 0; i < keep_n; i++) if (cache[e].key && keep[i] == cache[e].key) return i;
    return CACHE_N;
}

// A slot for a new picture: free, else the one worth least. A slot holding a picture still needed is only taken when
// `force` (a slide needs its pictures now), never one of the two `spare` keys.
static int slot_for(bool force, const void *spare1, const void *spare2)
{
    int best = -1, best_p = -1;
    for (int i = 0; i < CACHE_N; i++) {
        if (cache[i].key == spare1 || cache[i].key == spare2) { if (cache[i].key) continue; }
        int pr = cache[i].key ? priority(i) : CACHE_N + 1;          // free slots first
        if (cache[i].key && !cache[i].buf) pr = CACHE_N + 1;
        if (pr > best_p) { best_p = pr; best = i; }
    }
    if (best < 0 || (best_p < CACHE_N && !force)) return -1;
    return best;
}

static lv_draw_buf_t *get(const void *key, bool render, bool force, const void *spare)
{
    int e = find(key);
    if (e >= 0 && !cache[e].dirty) return cache[e].buf;
    if (!render || !paint_cb || !key) return NULL;
    if (e < 0 && (e = slot_for(force, spare, key)) < 0) return NULL;
    if (!cache[e].buf) {
        if (!room_for(1) && !force) return NULL;
        cache[e].buf = lv_draw_buf_create(DISP_W, DISP_H, LV_COLOR_FORMAT_RGB565, 0);
        if (!cache[e].buf) return NULL;
    }
    bool whole = cache[e].key != key || !cache[e].dirty; // a new picture; else only its out-of-date rows
    int y0 = whole ? 0 : cache[e].d0, y1 = whole ? DISP_H - 1 : cache[e].d1;
    cache[e].key = key;
    if (!paint_rows(e, y0, y1)) { cache[e].key = NULL; return NULL; }
    cache[e].dirty = false;
    return cache[e].buf;
}

lv_draw_buf_t *slide_cache_get(const void *key, bool render) { return get(key, render, false, NULL); }


// Rows y0..y1 of picture i are out of date (added to any rows already out of date; a render in progress restarts)
static void mark_rows(int i, int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > DISP_H - 1) y1 = DISP_H - 1;
    if (!cache[i].dirty) {
        cache[i].dirty = true;
        cache[i].dirty_since = esp_timer_get_time();
        cache[i].d0 = y0;
        cache[i].d1 = y1;
    } else {
        if (y0 < cache[i].d0) cache[i].d0 = y0;
        if (y1 > cache[i].d1) cache[i].d1 = y1;
    }
    cache[i].rows = cache[i].d0;
}

static void mark(int i) { mark_rows(i, 0, DISP_H - 1); }

void slide_cache_dirty(const void *key)
{
    for (int i = 0; i < CACHE_N; i++) if (!key || cache[i].key == key) mark(i);
}

void slide_cache_dirty_rows(const void *key, int y0, int y1)
{
    for (int i = 0; i < CACHE_N; i++) if (key && cache[i].key == key) mark_rows(i, y0, y1);
}

static const void *key_of(lv_obj_t *scr);

void slide_cache_dirty_hidden(bool (*except)(const void *key))
{
    const void *shown = key_of(lv_screen_active());
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].key && cache[i].key != shown && !(except && except(cache[i].key))) mark(i);
}


void slide_cache_keep(const void *const *keys, int n)
{
    keep_n = n > CACHE_N ? CACHE_N : n;
    for (int i = 0; i < keep_n; i++) keep[i] = keys[i];
}

bool slide_cache_idle_work(int quiet_ms)
{
    int64_t now = esp_timer_get_time();
    if (req_pending()) return false;
    bool quiet = now - last_change >= quiet_ms * 1000LL;
    for (int i = 0; i < keep_n; i++) {                   // the current screen first, then its neighbours
        int e = find(keep[i]);
        // Missing or dirty, once the screen is quiet; or dirty for 2 s even if it never is (the status page redraws
        // its ages every second: a picture a second old is fine for a slide, the real screen comes back after it);
        // or only a strip's worth of rows (a clock after the minute tick): at once, a drag right after finds it ready
        if (e < 0 ? quiet : cache[e].dirty && (quiet || now - cache[e].dirty_since > 2000000 ||
                                                cache[e].d1 - cache[e].d0 < STRIP)) {
            if (e < 0) {                                 // a slot for it: free, or holding a picture not needed now
                if ((e = slot_for(false, NULL, NULL)) < 0) return false;
                if (!cache[e].buf) {
                    if (!room_for(1)) return false;
                    cache[e].buf = lv_draw_buf_create(DISP_W, DISP_H, LV_COLOR_FORMAT_RGB565, 0);
                    if (!cache[e].buf) return false;
                }
                cache[e].key = keep[i];
                cache[e].dirty = false;
                mark(e);                                 // all of it
                cache[e].dirty_since = now;
            }
            // One strip at a time, so LVGL reads the touch in between (dirtied meanwhile: starts again), only over the
            // rows out of date (after the minute tick: a clock's ~40 rows instead of 466)
            int y0 = cache[e].rows, y1 = y0 + STRIP - 1 > cache[e].d1 ? cache[e].d1 : y0 + STRIP - 1;
            if (!paint_rows(e, y0, y1)) { cache[e].key = NULL; return false; }
            cache[e].rows = y1 + 1;
            if (cache[e].rows > cache[e].d1) cache[e].dirty = false;
            return true;
        }
    }
    return false;
}

static const void *key_of(lv_obj_t *scr) { return key_cb ? key_cb(scr) : scr; }

/* The picture of the screen shown follows the panel: every area LVGL sends (display flush hook) is copied into it, so
 * a redraw of the screen shown doesn't make it out of date (it did: after any change, the clock's minute included,
 * the next drag or list scroll waited ~0.1 s for a new one). Only when the screen shown changes before LVGL has drawn
 * its last redraw (a screen load right after a change) does that redraw miss the picture: then it is marked. */
static void flushed(const lv_area_t *a, const uint8_t *px)
{
    if (painting) return;
    int e = find(key_of(lv_screen_active()));
    if (e < 0 || !cache[e].buf) return;
    lv_draw_buf_t *b = cache[e].buf;
    int w = a->x2 - a->x1 + 1, x1 = a->x1 < 0 ? 0 : a->x1, x2 = a->x2 > DISP_W - 1 ? DISP_W - 1 : a->x2;
    for (int y = a->y1 < 0 ? 0 : a->y1; y <= a->y2 && y < DISP_H; y++)
        memcpy(b->data + y * b->header.stride + x1 * 2, px + ((y - a->y1) * w + x1 - a->x1) * 2, (x2 - x1 + 1) * 2);
}

static void invalidated(lv_event_t *e)
{
    if (painting) return;
    last_change = esp_timer_get_time();
    if (committing) return;
    const void *k = key_of(lv_screen_active());
    if (inv_pending && inv_key != k) slide_cache_dirty(inv_key);   // shown no more: its last redraw never came
    inv_key = k;
    inv_pending = true;
}

static void rendered(lv_event_t *e)
{
    if (inv_pending && inv_key != key_of(lv_screen_active())) slide_cache_dirty(inv_key);
    inv_pending = false;
}

void slide_cache_init(slide_paint_cb_t paint, slide_key_cb_t key)
{
    paint_cb = paint;
    key_cb = key;
    lv_display_t *d = lv_display_get_default();
    lv_display_add_event_cb(d, invalidated, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(d, rendered, LV_EVENT_RENDER_READY, NULL);
    display_set_flush_hook(flushed);
}

// After a slide: the real screen goes back up; LVGL repaints all of it (into its picture too: the same)
static void shown(lv_obj_t *scr)
{
    lv_screen_load(scr);
    lv_obj_invalidate(scr);
}

/* ---------- screen changes (lv_screen_load_anim replacement) ---------- */

static struct { lv_obj_t *to; lv_screen_load_anim_t how; uint32_t ms; bool queued; } req;

static void slide_start(void *unused);

// Called from event handlers deep in the stack: the pictures are rendered on the next LVGL cycle (lv_async_call), at
// the top of the LVGL task like a normal frame (a full render needs ~8 KB of stack).
void slide_screen(lv_obj_t *to, lv_screen_load_anim_t how, uint32_t ms)
{
    req.to = to; req.how = how; req.ms = ms;
    if (!req.queued) { req.queued = true; lv_async_call(slide_start, NULL); }
}

static void slide_start(void *unused)
{
    req.queued = false;
    lv_obj_t *to = req.to, *from = lv_screen_active();
    lv_screen_load_anim_t how = req.how;
    uint32_t ms = req.ms;
    if (to == from) return;
    // MOVE_LEFT / MOVE_TOP: the new screen comes from the right / below: it is "next" and the current one moves away
    int dir = how == LV_SCR_LOAD_ANIM_MOVE_LEFT || how == LV_SCR_LOAD_ANIM_MOVE_TOP ? 1 :
              how == LV_SCR_LOAD_ANIM_MOVE_RIGHT || how == LV_SCR_LOAD_ANIM_MOVE_BOTTOM ? -1 : 0;
    bool vertical = how == LV_SCR_LOAD_ANIM_MOVE_TOP || how == LV_SCR_LOAD_ANIM_MOVE_BOTTOM;
    slide_phase = 1;
    int64_t t0 = esp_timer_get_time();
    // Pictures from the cache (rendered now if missing, in the least useful slot: no extra memory; a 5-slot cache left
    // no room for one-off pictures and opening the hourly view fell back to the plain animation)
    const void *kf = key_of(from), *kt = key_of(to);
    lv_draw_buf_t *bc = dir ? get(kf, true, true, kt) : NULL;
    lv_draw_buf_t *bn = bc ? get(kt, true, true, kf) : NULL;
    if (!bc || !bn) {
        if (dir) ESP_LOGW(TAG, "%s: plain animation", room_for(1) ? "pictures failed" : "PSRAM busy");
        lv_screen_load_anim(to, how, ms, 0, false);
        slide_phase = 0;
        return;
    }
    int64_t t1 = esp_timer_get_time();
    frame_t f = { .cur = bc->data, .stride = bc->header.stride, .vertical = vertical };
    if (dir > 0) f.next = bn->data; else f.prev = bn->data;
    slide_phase = 5;
    int frames = animate(&f, -dir * (vertical ? DISP_H : DISP_W), ms);
    int64_t t2 = esp_timer_get_time();
    slide_phase = 6;
    shown(to);
    touch_resync(false);
    ESP_LOGI(TAG, "pictures %lld ms, %d frames in %lld ms (%.0f fps)", (t1 - t0) / 1000, frames, (t2 - t1) / 1000,
             frames * 1e6f / (t2 - t1));
    slide_phase = 0;
}

bool slide_running(void) { return req.queued; }

/* ---------- drags that follow the finger ---------- */

static struct {
    bool queued, vertical;
    int x0, y0;                          // where the finger went down
    int x1, y1;                          // where it was when the drag was recognised
    slide_neighbour_cb_t neighbour;
    slide_commit_cb_t commit;
    void *user;
} drag;

static struct { lv_obj_t *list; int y0, y1; int64_t t0, t1; bool queued; } scroll;   // a list scroll (below)
static struct { lv_obj_t *img; const uint16_t *src; int32_t from, to; uint32_t ms; slide_swipe_cb_t swipe;
                bool queued; } zoomq;                                  // a zoom (below)

static struct { const void *from, *to; int dir; bool vertical; slide_done_cb_t done; void *user; bool queued; } pageq;

static bool req_pending(void) { return req.queued || drag.queued || scroll.queued || zoomq.queued || pageq.queued; }

bool slide_screen_busy(void) { return req_pending() || touch_idle_ms() < 500; }

static void drag_run(void *unused);

bool slide_drag(bool vertical, int x0, int y0, int x1, int y1, slide_neighbour_cb_t neighbour,
                slide_commit_cb_t commit, void *user)
{
    if (req_pending()) return false;
    drag.queued = true;
    drag.vertical = vertical;
    drag.x0 = x0;
    drag.y0 = y0;
    drag.x1 = x1;
    drag.y1 = y1;
    drag.neighbour = neighbour;
    drag.commit = commit;
    drag.user = user;
    lv_async_call(drag_run, NULL);
    return true;
}

bool slide_drag_running(void) { return drag.queued; }

static void drag_run(void *unused)
{
    const int S = drag.vertical ? DISP_H : DISP_W;
    lv_obj_t *cur = lv_screen_active();
    const void *ckey = key_of(cur);
    const void *nkey[2] = {NULL, NULL};                 // neighbours: [0] = prev (side -1), [1] = next (side +1)
    lv_draw_buf_t *bs[2] = {NULL, NULL};
    bool tried[2] = {false, false};
    slide_phase = 11;
    int64_t t0 = esp_timer_get_time();
    bool cached = slide_cache_get(ckey, false) != NULL;
    lv_draw_buf_t *bc = slide_cache_get(ckey, true);
    if (!bc) {
        // No picture (PSRAM busy): still a drag, decided on release, the switch is just not animated
        ESP_LOGW(TAG, "drag: PSRAM busy, no animation");
        int x = drag.x1, y = drag.y1, nx, ny, r, errs = 0;
        int64_t until = t0 + FINGER_WAIT_US;
        while ((r = finger(&nx, &ny, &errs)) != 0 && esp_timer_get_time() < until) {
            if (r > 0) { x = nx; y = ny; }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (r) ESP_LOGW(TAG, "drag: finger still down after %lld ms, giving up", FINGER_WAIT_US / 1000);
        int raw = drag.vertical ? y - drag.y0 : x - drag.x0;
        if (abs(raw) > S / 4 && drag.commit) drag.commit(raw > 0 ? -1 : 1, drag.user);   // commit ignores an end
        touch_resync(r == 0);                                // as after an animated drag (else the next swipe is lost)
        drag.queued = false;
        slide_phase = 0;
        return;
    }
    frame_t f = { .cur = bc->data, .stride = bc->header.stride, .vertical = drag.vertical };
    int x = drag.x1, y = drag.y1, frames = 0, rendered = cached ? 0 : 1, raw = 0, samples = 0;
    // The neighbour the finger is heading to, before the first frame: a quick flick may be over by then
    int first = (drag.vertical ? y - drag.y0 : x - drag.x0) > 0 ? -1 : 1, fi = first < 0 ? 0 : 1;
    tried[fi] = true;
    nkey[fi] = drag.neighbour ? drag.neighbour(first, drag.user) : NULL;
    if (nkey[fi]) {
        if (!slide_cache_get(nkey[fi], false)) rendered++;
        bs[fi] = get(nkey[fi], true, true, ckey);            // takes the least useful slot if it must
    }
    if (fi == 0) f.prev = bs[0] ? bs[0]->data : NULL; else f.next = bs[1] ? bs[1]->data : NULL;
    int64_t t_prev = esp_timer_get_time(), t_last = t_prev, t_first = 0;
    int pos_prev = 0, pos_last = 0, errs = 0;
    // For the log line: what a real finger does (the harness's synthetic drags never hold): reads held by a bus error /
    // an unconfirmed "up", the longest run of held reads, the longest gap between two frames, the longest the finger
    // stood still (espforge LESSONS L163: two tries with the user's finger located both stalls this way)
    int held_err = 0, held_up = 0, still_raw = 0;
    int64_t hold_since = 0, hold_max = 0, frame_at = 0, gap_max = 0, still_since = 0, still_max = 0;
    slide_phase = 12;
    for (;;) {
        int nx, ny, r = finger(&nx, &ny, &errs);
        // Finger up: a clean release or 5 silent reads in a row (~75 ms, finger()). Else the drag never ended.
        if (r == 0) break;
        if (esp_timer_get_time() - t0 > FINGER_FOLLOW_US) { ESP_LOGW(TAG, "drag: finger down for 20 s, ending it"); break; }
        if (r > 0) { x = nx; y = ny; samples++; }
        raw = drag.vertical ? y - drag.y0 : x - drag.x0;     // > 0: towards prev (it comes in from the left / top)
        int64_t tnow = esp_timer_get_time();
        if (raw != still_raw || !still_since) { still_since = tnow; still_raw = raw; }     // the finger itself
        else if (tnow - still_since > still_max) still_max = tnow - still_since;
        // Held (a read error, or an "up" not confirmed yet: up to UP_HOLD_US): the page goes on at the finger's last
        // speed instead of standing still, at most 80 ms worth. It stood still ~60 ms, then snapped, at the end of
        // every swipe (espforge LESSONS L162: "finger still max" 74 -> 15 ms there).
        if (r < 0) {
            if (r == -1) held_err++; else held_up++;
            if (!hold_since) hold_since = tnow;
            if (tnow - hold_since > hold_max) hold_max = tnow - hold_since;
            if (t_last > t_prev) {
                int64_t ahead = tnow - t_last;
                if (ahead > 80000) ahead = 80000;
                raw = pos_last + (int)((pos_last - pos_prev) * (float)ahead / (t_last - t_prev));
            }
        } else hold_since = 0;
        int side = raw > 0 ? -1 : raw < 0 ? 1 : 0, i = side < 0 ? 0 : 1;
        if (side && !tried[i]) {                             // the neighbour on this side, the first time it's needed
            tried[i] = true;
            nkey[i] = drag.neighbour ? drag.neighbour(side, drag.user) : NULL;
            if (nkey[i]) {
                if (!slide_cache_get(nkey[i], false)) rendered++;
                bs[i] = slide_cache_get(nkey[i], true);      // ready already, unless it changed or memory is short
            }
            if (i == 0) f.prev = bs[0] ? bs[0]->data : NULL;
            else f.next = bs[1] ? bs[1]->data : NULL;
        }
        int off;
        if (side && bs[i]) off = raw < -S ? -S : raw > S ? S : raw;   // follows the finger
        else {                                                         // no neighbour: resists (LVGL-like bounce)
            off = raw / 3;
            if (off > S / 5) off = S / 5;
            if (off < -S / 5) off = -S / 5;
        }
        int64_t now = esp_timer_get_time();
        // (fresh readings only: a bridged "up" or the cached reading between two chip reads (every 10 ms) repeats the
        // point, and a flick measured 0 px/ms: the first day swipe to Monday snapped back, v1.12.1-fix.26)
        if (r > 0 && touch_fresh() && now - t_last > 8000) { t_prev = t_last; pos_prev = pos_last; t_last = now; pos_last = raw; }
        show(&f, off);
        int64_t done = esp_timer_get_time();
        if (frame_at && done - frame_at > gap_max) gap_max = done - frame_at;
        frame_at = done;
        if (!frames++) t_first = done;
    }
    // Finger up: on to the neighbour if dragged past a third or flicked towards it, else back
    float vel = t_last > t_prev ? (float)(pos_last - pos_prev) / ((t_last - t_prev) / 1000.0f) : 0;   // px/ms
    int side = f.off > 0 ? -1 : f.off < 0 ? 1 : 0, si = side < 0 ? 0 : 1;
    bool have = side && bs[si];
    bool go = have && (abs(f.off) > S / 3 || (abs(f.off) > 24 && vel * f.off > 0 && fabsf(vel) > 0.35f));
    // Lifted before the first frame (a flick while a picture was being rendered): a flick that way, if there's a
    // neighbour (the screen only moved the 10-16 px that made it a drag; judged by that, it bounced back)
    if (!samples && bs[fi]) { side = first; si = fi; have = true; go = true; }
    // The neighbour's picture failed (PSRAM busy) but the finger went far: switch anyway, without the animation. By the
    // finger's distance: the screen itself only moved a resisting fifth (judged by that, it never switched).
    bool blind = side && !have && nkey[si] && abs(raw) > S / 4;
    int to = go ? (side < 0 ? S : -S) : 0;
    int ms = 60 + 220 * abs(to - f.off) / S;
    frames += animate(&f, to, ms);
    int64_t t1 = esp_timer_get_time();
    slide_phase = 16;
    if ((go || blind) && drag.commit) {
        // The switch scrolls the pager (SCROLL_BEGIN, layout): LVGL redraws the page being left, whose content didn't
        // change. Counted as a change, its picture went out of date at every switch, and a drag back soon after
        // waited ~0.2 s for it. Content changes made meanwhile still mark their pictures (slide_cache_dirty).
        committing = true;
        drag.commit(side, drag.user);                        // loads the new screen / switches the page
        committing = false;
        lv_obj_invalidate(lv_screen_active());
    } else {
        lv_obj_invalidate(cur);
    }
    touch_resync(true);
    // Frame rate from the first frame (the time before it, rendering pictures, is the "first frame after" figure).
    // No frame while the finger was down: it lifted while the pictures were being rendered (a jump, not a drag).
    int64_t ts = t_first ? t_first : t0;
    // (the part after " | " is for people reading the log; the harness reads the part before it)
    if (t_first) ESP_LOGI(TAG, "drag: first frame after %lld ms (%d pictures rendered), %d frames in %lld ms (%.0f fps), %s"
                          " | gap max %lld ms, held reads %d err %d up (longest %lld ms), finger still max %lld ms, "
                          "samples %d, %.2f px/ms",
                          (t_first - t0) / 1000, rendered, frames, (t1 - ts) / 1000, frames * 1e6f / (t1 - ts),
                          go || blind ? (side < 0 ? "to prev" : "to next") : "back", gap_max / 1000, held_err, held_up,
                          hold_max / 1000, still_max / 1000, samples, vel);
    else ESP_LOGW(TAG, "drag: finger up before the first frame (%d pictures rendered in %lld ms), %s", rendered,
                  (t1 - t0) / 1000, go || blind ? (side < 0 ? "to prev" : "to next") : "back");
    drag.queued = false;
    slide_phase = 0;
}

/* ---------- a page change decided elsewhere ----------
 * The settings page picked another place: the place pager scrolled there with LVGL's animation, which redraws the
 * whole screen every frame (~10 fps on the weather screen: "sluggish"). Now the two pages' pictures slide like the end
 * of a drag (~65 fps), then done() moves the pager there without an animation. */

static void page_run(void *unused)
{
    const int S = pageq.vertical ? DISP_H : DISP_W;
    slide_phase = 41;
    int64_t t0 = esp_timer_get_time();
    lv_draw_buf_t *bc = get(pageq.from, true, true, pageq.to);
    lv_draw_buf_t *bn = bc ? get(pageq.to, true, true, pageq.from) : NULL;
    int frames = 0;
    int64_t t1 = esp_timer_get_time();
    if (bc && bn) {
        frame_t f = { .cur = bc->data, .stride = bc->header.stride, .vertical = pageq.vertical };
        if (pageq.dir > 0) f.next = bn->data; else f.prev = bn->data;
        slide_phase = 42;
        frames = animate(&f, -pageq.dir * S, 300);
    }
    int64_t t2 = esp_timer_get_time();
    committing = true;                                   // the pager moving is not a change of the pages
    if (pageq.done) pageq.done(pageq.user);
    committing = false;
    lv_obj_invalidate(lv_screen_active());
    touch_resync(false);
    if (frames) ESP_LOGI(TAG, "page: pictures %lld ms, %d frames in %lld ms (%.0f fps)", (t1 - t0) / 1000, frames,
                         (t2 - t1) / 1000, frames * 1e6f / (t2 - t1));
    else ESP_LOGW(TAG, "page: no pictures (PSRAM busy), no animation");
    pageq.queued = false;
    slide_phase = 0;
}

bool slide_page(const void *from, const void *to, bool vertical, int dir, slide_done_cb_t done, void *user)
{
    if (req_pending() || !from || !to || from == to || !dir) return false;
    pageq.from = from;
    pageq.to = to;
    pageq.vertical = vertical;
    pageq.dir = dir;
    pageq.done = done;
    pageq.user = user;
    pageq.queued = true;
    lv_async_call(page_run, NULL);
    return true;
}

/* ---------- list scrolling ----------
 * LVGL redraws all of a scrolling list for every frame (35-50 ms: 17-22 fps). Here each frame moves the rows already
 * on screen within the picture of the screen shown (which matches the panel, see flushed()), has LVGL render only the
 * rows that come into view, and sends just the list's rectangle to the panel. LVGL's own scroll position is kept up to
 * date (lv_obj_scroll_by_raw) so it renders those rows right and carries on from there afterwards; its redraw requests
 * are dropped at the end (the panel and the picture already show the result). Like LVGL: the list follows the finger,
 * goes on after a flick and slows down, resists past the ends and springs back; a touch stops it. */

// The scrollable object under x,y with somewhere to scroll vertically, or NULL
lv_obj_t *slide_scroll_target(lv_obj_t *scr, int x, int y)
{
    lv_point_t p = {x, y};
    for (lv_obj_t *o = lv_indev_search_obj(scr, &p); o && o != scr; o = lv_obj_get_parent(o)) {
        if (!lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) || !(lv_obj_get_scroll_dir(o) & LV_DIR_VER)) continue;
        if (lv_obj_get_scroll_top(o) > 0 || lv_obj_get_scroll_bottom(o) > 0) return o;
    }
    return NULL;
}

static void scroll_run(void *unused);
static slide_sideways_cb_t sideways_cb;
void slide_scroll_on_sideways(slide_sideways_cb_t cb) { sideways_cb = cb; }

bool slide_scroll(lv_obj_t *list, int y0, int64_t t0, int y1)
{
    if (req_pending()) return false;
    int e = find(key_of(lv_screen_active()));
    if (e < 0 || cache[e].dirty || !cache[e].buf) return false;   // no exact picture of the screen: LVGL scrolls
    scroll.list = list;
    scroll.y0 = y0;
    scroll.t0 = t0;
    scroll.y1 = y1;
    scroll.t1 = esp_timer_get_time();
    scroll.queued = true;
    lv_async_call(scroll_run, NULL);
    return true;
}

static void scroll_fill(int x0, int w, int y0, int n, void *dst, void *user)
{
    const lv_draw_buf_t *p = user;
    uint8_t *d = dst;
    for (int y = y0; y < y0 + n; y++, d += w * 2) copy_swap(d, p->data + y * p->header.stride + x0 * 2, w);
}

typedef struct {
    lv_obj_t *scr, *list;
    lv_draw_buf_t *pic;                  // the screen shown
    lv_draw_buf_t *strip;                // the rows coming into view, rendered before the frame is sent
    int d, in0, in1;                     // this frame: rows move by d (> 0: up); rows in0..in1 come from the strip
    int last;                            // the last move's way (> 0: up), for scroll_settle
    int up, down;                        // how far its text reaches past a label's box (render_rows_reach)
    lv_area_t r;                         // the list on screen: the rows that move
    int shown;                           // the scroll position the picture shows
    int frames;
    int64_t us_move, us_render, us_send; // time per part (log)
    int64_t us_settle;                   // scroll_settle, once at the end (log)
} scroller_t;

// One pass per frame: each row of the list area is moved within the picture (or taken from the strip) and sent.
// Moving rows down in place needs the bottom rows first: then the bands go bottom up and each band's rows too.
// From the strip: the rows coming in, and the s->up rows above them and s->down below. Those were drawn while the next
// label was still past the list's edge (LVGL clips it to the list), so without its syllabics' tops, then moved: in
// Inuktitut a slow drag left 6-7 rows off every time (pictest), a quick flick 1-2 rows one time in four (October 6).
static void scroll_move_fill(int x0, int w, int y0, int n, void *dst, void *user)
{
    scroller_t *s = user;
    const lv_area_t *r = &s->r;
    uint32_t st = s->pic->header.stride, lw = lv_area_get_width(r) * 2;
    uint8_t *pic = s->pic->data;
    int s0 = s->in0 - s->up, s1 = s->in1 + s->down;   // rows taken from the strip
    for (int i = 0; i < n; i++) {
        int y = s->d < 0 ? y0 + n - 1 - i : y0 + i;
        if (y >= r->y1 && y <= r->y2) {
            const uint8_t *src = y >= s0 && y <= s1 ? s->strip->data + (y - s->in0 + ROW_MARGIN) * s->strip->header.stride
                                                    : pic + (y + s->d) * st;
            memcpy(pic + y * st + r->x1 * 2, src + r->x1 * 2, lw);
        }
        copy_swap((uint8_t *)dst + (y - y0) * w * 2, pic + y * st + x0 * 2, w);
    }
}

// Scroll to position `want`: LVGL's state, the picture (moved rows + rendered new ones), the panel
static void scroll_step(scroller_t *s, int want)
{
    int d = want - s->shown, h = lv_area_get_height(&s->r);
    if (!d) return;
    s->last = d;
    // Raw: lv_obj_scroll_to_y() stops at the ends, and past them (pulled, springing back) the picture moved while
    // the list didn't: the rows rendered for it repeated the edge (smeared graph labels, a doubled row). And not
    // lv_obj_scroll_by(): its SCROLL_BEGIN / END events bubble up the hourly view (pager, screen) and made LVGL lay
    // out the whole screen again on every frame (4.4 ms of the hourly list's 7.7)
    lv_obj_scroll_by_raw(s->list, 0, lv_obj_get_scroll_y(s->list) - want);
    const lv_area_t *r = &s->r;
    int64_t t0 = esp_timer_get_time(), t1 = t0, t2;
    if (abs(d) < h && s->strip && abs(d) <= (int)s->strip->header.h - 2 * ROW_MARGIN) {
        // The rows coming in (at the bottom when the content goes up), rendered into the strip; then one pass
        s->d = d;
        s->in0 = d > 0 ? r->y2 - d + 1 : r->y1;
        s->in1 = d > 0 ? r->y2 : r->y1 - d - 1;
        render_rows_reach(s->scr, s->strip, s->in0 - ROW_MARGIN, s->in0, s->in1, s->up, s->down);   // (+ margins)
        t2 = esp_timer_get_time();
        display_raw_area(r->x1, r->y1, r->x2, r->y2, d < 0, scroll_move_fill, s);
    } else {                                             // a jump (or no strip): the whole list, then send
        if (abs(d) >= h) render_rows_reach(s->scr, s->pic, 0, r->y1, r->y2, s->up, s->down);
        else {
            uint8_t *base = s->pic->data + r->x1 * 2;
            uint32_t st = s->pic->header.stride, w = lv_area_get_width(r) * 2;
            if (d > 0) for (int y = r->y1; y <= r->y2 - d; y++) memcpy(base + y * st, base + (y + d) * st, w);
            else for (int y = r->y2; y >= r->y1 - d; y--) memcpy(base + y * st, base + (y + d) * st, w);
            t1 = esp_timer_get_time();
            render_rows_reach(s->scr, s->pic, 0, d > 0 ? r->y2 - d + 1 : r->y1, d > 0 ? r->y2 : r->y1 - d - 1, s->up, s->down);
        }
        t2 = esp_timer_get_time();
        display_raw_area(r->x1, r->y1, r->x2, r->y2, false, scroll_fill, s->pic);
    }
    s->us_move += t1 - t0;
    s->us_render += t2 - t1;
    s->us_send += esp_timer_get_time() - t2;
    s->shown = want;
    s->frames++;
}

// After the last frame, the edge the list last moved away from. A label whose box has just gone past it is skipped
// by LVGL (clipped to the list), but the rows moved within the picture still hold what its text reached past the box:
// syllabics' tops above a label gone out at the bottom (a scroll back), descenders below one gone out at the top. A
// move the other way takes those rows from the strip, so only the last move's edge can be left off: its rows are
// rendered again, once, and sent (v1.14.3-rc.1: row 359, Settings' last, after one harness run's slow scroll back in 8).
static void scroll_settle(scroller_t *s)
{
    if (!s->last) return;
    int64_t t0 = esp_timer_get_time();
    const lv_area_t *r = &s->r;
    int y0 = s->last < 0 ? r->y2 - s->up + 1 : r->y1, y1 = s->last < 0 ? r->y2 : r->y1 + s->down - 1;
    render_rows_reach(s->scr, s->pic, 0, y0, y1, s->up, s->down);
    display_raw_area(0, y0 - s->up, DISP_W - 1, y1 + s->down, false, scroll_fill, s->pic);   // every row it changed
    s->us_settle = esp_timer_get_time() - t0;
}

static void scroll_run(void *unused)
{
    // Syllabics in a list only in Inuktitut (lists hold no place or network names): elsewhere Montserrat's smaller
    // reach, fewer rows rendered a frame (Settings 3.7 ms a frame before October 6, 5.4 with Inuktitut's everywhere)
    bool syl = i18n_lang() == LANG_IU;
    scroller_t s = { .scr = lv_screen_active(), .list = scroll.list, .up = syl ? ROW_UP : LATIN_UP,
                     .down = syl ? ROW_DOWN : LATIN_DOWN };
    lv_display_t *disp = lv_display_get_default();
    int e = find(key_of(s.scr));
    slide_phase = 21;
    if (e < 0 || cache[e].dirty || !lv_obj_is_valid(s.list)) { scroll.queued = false; slide_phase = 0; return; }
    s.pic = cache[e].buf;
    s.strip = lv_draw_buf_create(DISP_W, 96 + 2 * ROW_MARGIN, LV_COLOR_FORMAT_RGB565, 0);   // 112 KB; NULL: slower path
    int64_t t0 = esp_timer_get_time();
    // LVGL lets go of the touch (the row pressed under the finger, its own scroll if it had begun) and draws that now:
    // to the panel and, through flushed(), into the picture
    lv_indev_t *in = lv_indev_get_next(NULL);
    lv_indev_reset(in, NULL);
    lv_refr_now(disp);
    uint32_t inv0 = disp->inv_p;
    painting = true;                                     // from here the picture is kept up to date by this code
    lv_obj_get_coords(s.list, &s.r);
    lv_area_t screen = {0, 0, DISP_W - 1, DISP_H - 1};
    lv_area_intersect(&s.r, &s.r, &screen);
    const int H = lv_area_get_height(&s.r);
    s.shown = lv_obj_get_scroll_y(s.list);
    const int lo = s.shown - lv_obj_get_scroll_top(s.list), hi = s.shown + lv_obj_get_scroll_bottom(s.list);
    const int over = H / 5;                              // how far past an end it can be pulled
    slide_phase = 22;

    int ref_y = scroll.y1, ref_pos = s.shown, pos = s.shown, fy = scroll.y1, errs = 0, moved = 0;
    float fpos = pos, v = 0;
    struct { int64_t t; int p; } smp[8];                 // finger samples (time, position) for the flick speed:
    smp[0].t = scroll.t0;                                // the press and the 10 px that made it a scroll come first
    smp[0].p = ref_pos + (scroll.y1 - scroll.y0);        // (a flick can be over by now)
    smp[1].t = scroll.t1;
    smp[1].p = ref_pos;
    int ns = 2;
    bool down = true;
    // A touch while coasting stops the list; it moves again only once the finger has moved 10 px (as a new touch is decided,
    // new touch), and a sideways one goes to sideways_cb (the hourly view: the next day). It used to follow the finger
    // vertically at once: a day swipe made before the list had stopped was swallowed.
    bool deciding = false, sideways = false, maybe_up = false;   // maybe_up: coasting on an "up" not confirmed yet
    int lifts = 0, touches = 0, err0 = err_lifts, read_errs = 0, brief0 = brief_ups;
    brief_max = 0;
    int64_t last_read = 0, gap_max = 0;
    int tx0 = 0, ty0 = 0, sx1 = 0, sy1 = 0;
    int64_t t = esp_timer_get_time(), touched_at = t;
    for (;;) {
        int x, y, r = finger(&x, &y, &errs);
        int64_t now = esp_timer_get_time();
        if (down) {
            if (r == -1) read_errs++;
            if (last_read && now - last_read > gap_max) gap_max = now - last_read;
            last_read = now;
        } else last_read = 0;
        if (down && deciding && r != 0) {
            if (r < 0 || (abs(x - tx0) < 10 && abs(y - ty0) < 10)) { vTaskDelay(1); continue; }
            deciding = false;
            if (abs(x - tx0) > abs(y - ty0) && sideways_cb) { sideways = true; sx1 = x; sy1 = y; break; }
            ref_y = ty0;                                 // vertical: the list follows from where the touch went down
            fy = y;                                      // (catching up the 10 px it took to decide), and its flick
            ref_pos = pos;                               // speed counts from the touch, as a first flick's
            smp[0].t = touched_at;
            smp[0].p = ref_pos;
            smp[1].t = now;
            smp[1].p = ref_pos + (ty0 - y);
            ns = 2;
        }
        if (down) {
            bool stuck = now - touched_at > FINGER_FOLLOW_US;
            if (stuck) ESP_LOGW(TAG, "scroll: finger down for 20 s, ending it");
            if (r == 0 || r == -2 || stuck) {            // lifted (or 5 silent reads: the chip NACKs when idle), or
                maybe_up = r == -2;                      // maybe: coast now, and follow again if the finger is back
                if (!maybe_up) lifts++;
                down = false;
                deciding = false;
                v = 0;                                   // speed over the last ~80 ms (0 if the finger stopped)
                if (ns) {                                // (none yet: touched again and lifted at once)
                    int nk = (ns - 1) % 8;
                    for (int i = ns - 2; i >= 0 && i >= ns - 8; i--) {
                        int k = i % 8;
                        int64_t span = smp[nk].t - smp[k].t;
                        if (span > 80000) break;
                        if (span >= 10000) v = (float)(smp[nk].p - smp[k].p) / (span / 1000.0f);
                    }
                    if (now - smp[nk].t > 80000 + UP_HOLD_US) v = 0;   // (the lift is known UP_HOLD_US late)
                }
                fpos = pos;
                t = now;
                if (stuck) break;
                continue;
            }
            if (r > 0) fy = y;                           // (a read error: keep the last point)
            int raw = ref_pos + (ref_y - fy);            // the content follows the finger
            if (r > 0 && touch_fresh() && (!ns || now - smp[(ns - 1) % 8].t >= 8000)) { smp[ns % 8].t = now; smp[ns % 8].p = raw; ns++; }   // (real readings)
            int want = raw < lo ? lo - (lo - raw) / 3 : raw > hi ? hi + (raw - hi) / 3 : raw;   // resists past an end
            if (want < lo - over) want = lo - over;
            if (want > hi + over) want = hi + over;
            if (want == s.shown) { vTaskDelay(1); continue; }
            moved += abs(want - pos);
            pos = want;
            scroll_step(&s, pos);
            continue;
        }
        if (maybe_up && r >= 0) {                        // the "up" settled: a real lift, or the same finger back
            maybe_up = false;
            if (r == 0) lifts++;
            else if (r > 0) {                            // it never lifted (a brief "up"): follow it again from here
                down = true;
                ref_y = fy = y;
                ref_pos = pos;
                smp[0].t = now;
                smp[0].p = pos;
                ns = 1;
                continue;
            }
        }
        if (r > 0) {                                     // touched while moving: stop, and see where the finger goes
            touches++;
            down = deciding = true;
            touched_at = now;
            tx0 = x;
            ty0 = y;
            ref_y = fy = y;
            ref_pos = pos;
            ns = 0;
            continue;
        }
        float dt = (now - t) / 1000.0f;
        t = now;
        bool past = pos < lo || pos > hi;
        if (past) v *= expf(-dt / 30.0f);                // past an end: brake hard...
        else v *= expf(-dt / 300.0f);                    // a flick goes on ~v x 300 ms, as LVGL's (10% per frame)
        if (past ? fabsf(v) < 0.1f : fabsf(v) < 0.03f) {
            if (!past && maybe_up) { vTaskDelay(1); continue; }   // stopped, but the finger may still be there
            if (!past) break;
            // ...then spring back to the end (ease out, 220 ms). A touch meanwhile is one during the coast (it read
            // no finger: a day swipe made as a flick reached the end of the short hours list was lost)
            int from = pos, to = pos < lo ? lo : hi;
            int64_t start = esp_timer_get_time();
            bool touched = false;
            for (;;) {
                float k = (esp_timer_get_time() - start) / 220000.0f;
                if (k > 1) k = 1;
                float ease = 1 - (1 - k) * (1 - k) * (1 - k);
                pos = from + (int)((to - from) * ease);
                scroll_step(&s, pos);
                if (k >= 1) break;
                if (finger(&x, &y, &errs) > 0) { touched = true; touches++; break; }
            }
            if (!touched && maybe_up) { fpos = pos; v = 0; continue; }   // (undecided "up": wait at the end)
            if (!touched) break;
            if (maybe_up) {                              // the finger never lifted: follow it again
                maybe_up = false;
                down = true;
                ref_y = fy = y;
                ref_pos = pos;
                fpos = pos;
                v = 0;
                smp[0].t = esp_timer_get_time();
                smp[0].p = pos;
                ns = 1;
                continue;
            }
            down = deciding = true;
            touched_at = esp_timer_get_time();
            tx0 = x;
            ty0 = y;
            ref_y = fy = y;
            ref_pos = pos;
            fpos = pos;
            v = 0;
            ns = 0;
            continue;
        }
        fpos += v * dt;
        if (fpos < lo - over) { fpos = lo - over; v = 0; }
        if (fpos > hi + over) { fpos = hi + over; v = 0; }
        if ((int)fpos == pos) { vTaskDelay(1); continue; }
        moved += abs((int)fpos - pos);
        pos = (int)fpos;
        scroll_step(&s, pos);
    }
    scroll_settle(&s);
    painting = false;
    disp->inv_p = inv0;                                  // LVGL's redraw requests for the list: already on the panel
    if (s.strip) lv_draw_buf_destroy(s.strip);
    last_change = esp_timer_get_time();
    if (!sideways) touch_resync(true);
    int64_t t1 = esp_timer_get_time();
    int f = s.frames ? s.frames : 1;
    ESP_LOGI(TAG, "scroll: %d frames in %lld ms, moved %d px, now at %d of %d..%d; per frame: move %.1f, render %.1f, "
             "send %.1f ms, then the edge's rows %.1f ms%s; finger: %d lifts, %d silences, %d touches again, %d brief ups "
             "bridged (longest %lld ms), %d read errors, longest gap %lld ms", s.frames, (t1 - t0) / 1000, moved, pos,
             lo, hi, s.us_move / 1000.0f / f, s.us_render / 1000.0f / f, s.us_send / 1000.0f / f,
             s.us_settle / 1000.0f, sideways ? "; then a sideways touch" : "", lifts, err_lifts - err0, touches,
             brief_ups - brief0, brief_max / 1000, read_errs, gap_max / 1000);
    scroll.queued = false;
    slide_phase = 0;
    // The sideways touch: to its drag (which reads the finger from here), else LVGL ignores it until it lifts
    if (sideways && !sideways_cb(tx0, ty0, sx1, sy1)) touch_resync(false);
}

// Test console "pictest": the picture of the screen shown against a fresh rendering, 32 rows at a time. Returns the
// number of rows that differ (first one in *first), -1 without a picture, -2 without memory, -3 out of date.
static int picture_check(int *first)
{
    *first = -1;
    lv_refr_now(NULL);                                   // pending redraws first (a live screen: the status ages)
    int e = find(key_of(lv_screen_active()));
    if (e < 0 || !cache[e].buf) return -1;
    if (cache[e].dirty) return -3;
    lv_draw_buf_t *strip = lv_draw_buf_create(DISP_W, 32 + 2 * ROW_MARGIN, LV_COLOR_FORMAT_RGB565, 0);
    if (!strip) return -2;
    lv_display_t *disp = lv_display_get_default();
    uint32_t inv0 = disp->inv_p;
    painting = true;
    int bad = 0;
    for (int y0 = 0; y0 < DISP_H; y0 += 32) {
        int y1 = y0 + 31 > DISP_H - 1 ? DISP_H - 1 : y0 + 31;
        render_rows(lv_screen_active(), strip, y0 - ROW_MARGIN, y0, y1);
        for (int y = y0; y <= y1; y++) {
            if (!memcmp(strip->data + (y - y0 + ROW_MARGIN) * strip->header.stride, cache[e].buf->data + y * cache[e].buf->header.stride,
                        DISP_W * 2)) continue;
            if (*first < 0) *first = y;
            bad++;
        }
    }
    painting = false;
    disp->inv_p = inv0;
    lv_draw_buf_destroy(strip);
    return bad;
}

static void check_cb(void *unused)
{
    int first, bad = picture_check(&first);
    ESP_LOGI(TAG, "pictest rows_differ=%d first=%d", bad, first);
}

void slide_picture_check_async(void) { lv_async_call(check_cb, NULL); }   // in the LVGL task (~8 KB of stack)

// Snapshot "picture": a copy of the picture of the screen shown, to compare with the screen (tools/snapshot.py)
lv_draw_buf_t *slide_picture_copy(void)
{
    int e = find(key_of(lv_screen_active()));
    return e < 0 || !cache[e].buf ? NULL : lv_draw_buf_dup(cache[e].buf);
}

/* ---------- image zoom (the radar) ----------
 * LVGL transforms the whole image for every frame of a zoom (lv_image_set_scale: ~100 ms a frame, ~10 fps). Here
 * each frame is scaled straight into the panel bands (nearest neighbour through row and column maps, as LVGL with
 * antialias off) and the screen's other objects are blended on top: rendered once at the start with their
 * transparency (the radar's pill, labels, ring and dot are 30-70 % opaque over the map) into a list of the pixels
 * they cover. At the end LVGL's own scale is set and it redraws the same picture (its flush updates slide.c's). */

// The pixels the overlays cover: position (y * DISP_W + x, 18 bits) and alpha (top 8 bits) in one word, colour apart:
// 6 bytes each, ~16k for the radar (pill, labels, ring, dot)
#define OVL_CAP 24000
static uint32_t *ovl;                    // pos | alpha << 24
static uint16_t *ovl_c;                  // RGB565
static int ovl_n;
static EXT_RAM_BSS_ATTR int ovl_row[DISP_H + 1];   // (PSRAM: internal RAM is short, its low point fell to 3 KB)

static inline uint16_t rgb565_of(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

// The visible children of scr but img, with their alpha: ARGB8888 strips on a transparent background. Per strip only
// the box the children cover in those rows is cleared, drawn and scanned: drawing the radar's objects takes ~36 ms
// (the range ring 19), and clearing and scanning whole 466-px strips took as long again (~110 ms in all before)
#define OVL_ROWS 32
static bool overlay_build(lv_obj_t *scr, lv_obj_t *img)
{
    ovl = heap_caps_malloc(OVL_CAP * sizeof(*ovl), MALLOC_CAP_SPIRAM);
    ovl_c = heap_caps_malloc(OVL_CAP * sizeof(*ovl_c), MALLOC_CAP_SPIRAM);
    lv_draw_buf_t *strip = lv_draw_buf_create(DISP_W, OVL_ROWS, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!ovl || !ovl_c || !strip) {
        free(ovl);
        free(ovl_c);
        ovl = NULL;
        ovl_c = NULL;
        if (strip) lv_draw_buf_destroy(strip);
        return false;
    }
    ovl_n = 0;
    lv_obj_update_layout(scr);
    lv_display_t *d = lv_obj_get_display(scr), *old = lv_refr_get_disp_refreshing();
    lv_layer_t *old_head = d->layer_head;
    for (int y0 = 0; y0 < DISP_H; y0 += OVL_ROWS) {
        int y1 = y0 + OVL_ROWS - 1 > DISP_H - 1 ? DISP_H - 1 : y0 + OVL_ROWS - 1;
        int bx1 = DISP_W, bx2 = -1;                      // the children's box in these rows (with their shadows etc.)
        for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
            lv_obj_t *c = lv_obj_get_child(scr, i);
            if (c == img || lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
            lv_area_t a;
            lv_obj_get_coords(c, &a);
            int32_t ext = lv_obj_get_ext_draw_size(c);
            lv_area_increase(&a, ext, ext);
            if (a.y2 < y0 || a.y1 > y1) continue;
            if (a.x1 < bx1) bx1 = a.x1;
            if (a.x2 > bx2) bx2 = a.x2;
        }
        if (bx1 < 0) bx1 = 0;
        if (bx2 > DISP_W - 1) bx2 = DISP_W - 1;
        if (bx2 < bx1) {                                 // nothing in these rows
            for (int y = y0; y <= y1; y++) ovl_row[y] = ovl_n;
            continue;
        }
        lv_area_t rows = {bx1, y0, bx2, y1}, in_buf = {bx1, 0, bx2, y1 - y0};
        lv_draw_buf_clear(strip, &in_buf);
        lv_layer_t layer;
        lv_memzero(&layer, sizeof(layer));
        layer.draw_buf = strip;
        layer.buf_area = (lv_area_t){0, y0, DISP_W - 1, y0 + OVL_ROWS - 1};
        layer.color_format = LV_COLOR_FORMAT_ARGB8888;
        layer._clip_area = rows;
        layer.phy_clip_area = rows;
        d->layer_head = &layer;
        lv_refr_set_disp_refreshing(d);
        for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
            lv_obj_t *c = lv_obj_get_child(scr, i);
            if (c != img && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) lv_obj_redraw(&layer, c);
        }
        while (layer.draw_task_head) {
            lv_draw_dispatch_wait_for_request();
            lv_draw_dispatch();
        }
        d->layer_head = old_head;
        lv_refr_set_disp_refreshing(old);
        for (int y = y0; y <= y1; y++) {
            ovl_row[y] = ovl_n;
            const uint32_t *p = (const uint32_t *)(strip->data + (y - y0) * strip->header.stride);   // A R G B
            for (int x = bx1; x <= bx2; x++) {
                uint32_t v = p[x];
                if (!(v >> 24) || ovl_n == OVL_CAP) continue;
                ovl_c[ovl_n] = rgb565_of(v >> 16, v >> 8, v);
                ovl[ovl_n++] = (uint32_t)(y * DISP_W + x) | (v & 0xFF000000u);
            }
        }
    }
    ovl_row[DISP_H] = ovl_n;
    lv_draw_buf_destroy(strip);
    return true;
}

static inline uint16_t blend565(uint16_t c, uint16_t m, int a)
{
    int r = (((c >> 11) & 31) * a + ((m >> 11) & 31) * (256 - a)) >> 8;
    int g = (((c >> 5) & 63) * a + ((m >> 5) & 63) * (256 - a)) >> 8;
    int b = ((c & 31) * a + (m & 31) * (256 - a)) >> 8;
    return (r << 11) | (g << 5) | b;
}

typedef struct { const uint16_t *src; int16_t col[DISP_W], row[DISP_H]; } zframe_t;

static void zoom_fill(int y0, int n, void *dst, void *user)
{
    const zframe_t *z = user;
    uint16_t *d = dst;
    for (int y = y0; y < y0 + n; y++, d += DISP_W) {
        const uint16_t *s = z->src + z->row[y] * DISP_W;
        for (int x = 0; x < DISP_W; x++) d[x] = s[z->col[x]];
        for (int k = ovl_row[y]; k < ovl_row[y + 1]; k++) {
            int x = (int)(ovl[k] & 0xFFFFFF) - y * DISP_W, a = ovl[k] >> 24;
            d[x] = blend565(ovl_c[k], d[x], a + (a >> 7));                // alpha 0..255 -> 0..256
        }
        copy_swap(d, d, DISP_W);                         // panel byte order, in place
    }
}

// Source pixel of each screen column / row at scale s (256 = 1x), pivot = the image's centre, as LVGL draws it
static void zoom_maps(zframe_t *z, int32_t s)
{
    const int px = DISP_W / 2, py = DISP_H / 2;
    for (int x = 0; x < DISP_W; x++) {
        int v = px + (int)(((int64_t)(x - px) * 256) / s);
        z->col[x] = v < 0 ? 0 : v > DISP_W - 1 ? DISP_W - 1 : v;
    }
    for (int y = 0; y < DISP_H; y++) {
        int v = py + (int)(((int64_t)(y - py) * 256) / s);
        z->row[y] = v < 0 ? 0 : v > DISP_H - 1 ? DISP_H - 1 : v;
    }
}

static void zoom_run(void *unused)
{
    lv_obj_t *img = zoomq.img, *scr = lv_obj_get_screen(img);
    const int32_t from = zoomq.from, to = zoomq.to;
    slide_phase = 31;
    int64_t t0 = esp_timer_get_time();
    if (scr != lv_screen_active() || !overlay_build(scr, img)) {     // left meanwhile, or no memory: no animation
        lv_image_set_scale(img, to);
        zoomq.queued = false;
        slide_phase = 0;
        return;
    }
    int64_t t1 = esp_timer_get_time();
    static EXT_RAM_BSS_ATTR zframe_t z;                  // (PSRAM, as ovl_row)
    z.src = zoomq.src;
    int frames = 0, x, y, x0 = 0, y0 = 0, xl = 0, yl = 0;
    bool down = false, touched = false;
    // The swipe that started this zoom may still be going on (LVGL's gesture fires before the finger lifts): not a
    // new one, or one swipe zoomed twice. It ends on a release or on the chip's silence (finger(): 5 failed reads);
    // a failed read alone kept it "on" and a second swipe during the zoom was taken for the first one (ignored).
    int errs = 0;
    bool old_touch = finger(&x, &y, &errs) != 0;
    slide_phase = 32;
    // LVGL doesn't read the touch meanwhile: a swipe made during the zoom (a second zoom right away) is read here
    for (;;) {
        int64_t el = esp_timer_get_time() - t1;
        float t = el >= zoomq.ms * 1000LL ? 1.0f : (float)el / (zoomq.ms * 1000.0f);
        float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);      // ease out
        zoom_maps(&z, from + (int32_t)((to - from) * e));
        display_raw_frame(zoom_fill, &z);
        frames++;
        int r = finger(&x, &y, &errs);
        if (old_touch) old_touch = r != 0;               // until it lifts
        else if (r > 0) { if (!down) { x0 = x; y0 = y; } down = touched = true; xl = x; yl = y; }
        else if (r == 0) down = false;
        if (t >= 1.0f) break;
    }
    for (int i = 0; down && i < 50; i++) {               // a swipe still going on: let it finish (at most 0.5 s)
        vTaskDelay(pdMS_TO_TICKS(10));
        int r = finger(&x, &y, &errs);
        if (r > 0) { xl = x; yl = y; } else if (r == 0) down = false;
    }
    int64_t t2 = esp_timer_get_time();
    free(ovl);
    free(ovl_c);
    ovl = NULL;
    ovl_c = NULL;
    lv_image_set_scale(img, to);                         // LVGL's state: it redraws the same last frame
    lv_obj_invalidate(scr);
    if (touched) touch_resync(!down);                    // LVGL starts afresh (no stray tap from that touch)
    ESP_LOGI(TAG, "zoom %ld -> %ld: overlays %d px in %lld ms, %d frames in %lld ms (%.0f fps)", (long)from, (long)to,
             ovl_n, (t1 - t0) / 1000, frames, (t2 - t1) / 1000, frames * 1e6f / (t2 - t1));
    zoomq.queued = false;
    slide_phase = 0;
    if (touched && !down && zoomq.swipe) zoomq.swipe(xl - x0, yl - y0);   // e.g. the next zoom
}

bool slide_zoom(lv_obj_t *img, const uint16_t *src, int32_t from, int32_t to, uint32_t ms, slide_swipe_cb_t swipe)
{
    if (req_pending() || lv_obj_get_screen(img) != lv_screen_active() || from < 256 || to < 256) return false;
    if (from == to) { lv_image_set_scale(img, to); return true; }   // nothing to animate (already at the closest)
    zoomq.img = img;
    zoomq.src = src;
    zoomq.from = from;
    zoomq.to = to;
    zoomq.ms = ms;
    zoomq.swipe = swipe;
    zoomq.queued = true;
    lv_async_call(zoom_run, NULL);
    return true;
}
