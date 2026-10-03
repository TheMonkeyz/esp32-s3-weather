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
 * 1 = down at x,y; 0 = up; -1 = a read error while down, not yet a release (keep the last point). */
#define FINGER_UP 5
static int finger(int *x, int *y, int *errs)
{
    int r = touch_get(x, y);
    if (r > 0) { *errs = 0; return 1; }
    if (r == 0) { *errs = FINGER_UP; return 0; }
    if (*errs < FINGER_UP) ++*errs;
    return *errs >= FINGER_UP ? 0 : -1;
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

bool slide_picture(lv_obj_t *scr, lv_draw_buf_t *dst) { return slide_picture_rows(scr, dst, 0, DISP_H - 1); }

// Rows y0..y1 of a screen into dst (a whole-screen RGB565 buffer): lv_snapshot_take_to_draw_buf() clipped to those
// rows, so a picture can be rendered a strip at a time (a whole one blocks LVGL for 60-180 ms: a quick swipe that
// started and ended meanwhile was never seen).
static bool render_rows(lv_obj_t *scr, lv_draw_buf_t *dst, int top, int y0, int y1)
{
    lv_obj_update_layout(scr);
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
    return true;
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

static bool req_pending(void) { return req.queued || drag.queued || scroll.queued || zoomq.queued; }

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
    slide_phase = 12;
    for (;;) {
        int nx, ny, r = finger(&nx, &ny, &errs);
        // Finger up: a clean release or 5 silent reads in a row (~75 ms, finger()). Else the drag never ended.
        if (r == 0) break;
        if (esp_timer_get_time() - t0 > FINGER_FOLLOW_US) { ESP_LOGW(TAG, "drag: finger down for 20 s, ending it"); break; }
        if (r > 0) { x = nx; y = ny; samples++; }
        raw = drag.vertical ? y - drag.y0 : x - drag.x0;     // > 0: towards prev (it comes in from the left / top)
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
        if (now - t_last > 8000) { t_prev = t_last; pos_prev = pos_last; t_last = now; pos_last = raw; }
        show(&f, off);
        if (!frames++) t_first = esp_timer_get_time();
    }
    // Finger up: on to the neighbour if dragged past a third or flicked towards it, else back
    float vel = t_last > t_prev ? (float)(pos_last - pos_prev) / ((t_last - t_prev) / 1000.0f) : 0;   // px/ms
    int side = f.off > 0 ? -1 : f.off < 0 ? 1 : 0, si = side < 0 ? 0 : 1;
    bool have = side && bs[si];
    bool go = have && (abs(f.off) > S / 3 || (abs(f.off) > 24 && vel * f.off > 0 && fabsf(vel) > 0.35f));
    // Lifted before the first frame (a flick while a picture was being rendered): a flick that way, if there's a
    // neighbour (the screen only moved the 16 px that made it a drag; judged by that, it bounced back)
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
    if (t_first) ESP_LOGI(TAG, "drag: first frame after %lld ms (%d pictures rendered), %d frames in %lld ms (%.0f fps), %s",
                          (t_first - t0) / 1000, rendered, frames, (t1 - ts) / 1000, frames * 1e6f / (t1 - ts),
                          go || blind ? (side < 0 ? "to prev" : "to next") : "back");
    else ESP_LOGW(TAG, "drag: finger up before the first frame (%d pictures rendered in %lld ms), %s", rendered,
                  (t1 - t0) / 1000, go || blind ? (side < 0 ? "to prev" : "to next") : "back");
    drag.queued = false;
    slide_phase = 0;
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
    lv_area_t r;                         // the list on screen: the rows that move
    int shown;                           // the scroll position the picture shows
    int frames;
    int64_t us_move, us_render, us_send; // time per part (log)
} scroller_t;

// One pass per frame: each row of the list area is moved within the picture (or taken from the strip) and sent.
// Moving rows down in place needs the bottom rows first: then the bands go bottom up and each band's rows too.
static void scroll_move_fill(int x0, int w, int y0, int n, void *dst, void *user)
{
    scroller_t *s = user;
    const lv_area_t *r = &s->r;
    uint32_t st = s->pic->header.stride, lw = lv_area_get_width(r) * 2;
    uint8_t *pic = s->pic->data;
    for (int i = 0; i < n; i++) {
        int y = s->d < 0 ? y0 + n - 1 - i : y0 + i;
        if (y >= r->y1 && y <= r->y2) {
            const uint8_t *src = y >= s->in0 && y <= s->in1 ? s->strip->data + (y - s->in0) * s->strip->header.stride
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
    // Raw: lv_obj_scroll_to_y() stops at the ends, and past them (pulled, springing back) the picture moved while
    // the list didn't: the rows rendered for it repeated the edge (smeared graph labels, a doubled row). And not
    // lv_obj_scroll_by(): its SCROLL_BEGIN / END events bubble up the hourly view (pager, screen) and made LVGL lay
    // out the whole screen again on every frame (4.4 ms of the hourly list's 7.7)
    lv_obj_scroll_by_raw(s->list, 0, lv_obj_get_scroll_y(s->list) - want);
    const lv_area_t *r = &s->r;
    int64_t t0 = esp_timer_get_time(), t1 = t0, t2;
    if (abs(d) < h && s->strip && abs(d) <= (int)s->strip->header.h) {
        // The rows coming in (at the bottom when the content goes up), rendered into the strip; then one pass
        s->d = d;
        s->in0 = d > 0 ? r->y2 - d + 1 : r->y1;
        s->in1 = d > 0 ? r->y2 : r->y1 - d - 1;
        render_rows(s->scr, s->strip, s->in0, s->in0, s->in1);
        t2 = esp_timer_get_time();
        display_raw_area(r->x1, r->y1, r->x2, r->y2, d < 0, scroll_move_fill, s);
    } else {                                             // a jump (or no strip): the whole list, then send
        if (abs(d) >= h) render_rows(s->scr, s->pic, 0, r->y1, r->y2);
        else {
            uint8_t *base = s->pic->data + r->x1 * 2;
            uint32_t st = s->pic->header.stride, w = lv_area_get_width(r) * 2;
            if (d > 0) for (int y = r->y1; y <= r->y2 - d; y++) memcpy(base + y * st, base + (y + d) * st, w);
            else for (int y = r->y2; y >= r->y1 - d; y--) memcpy(base + y * st, base + (y + d) * st, w);
            t1 = esp_timer_get_time();
            render_rows(s->scr, s->pic, 0, d > 0 ? r->y2 - d + 1 : r->y1, d > 0 ? r->y2 : r->y1 - d - 1);
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

static void scroll_run(void *unused)
{
    scroller_t s = { .scr = lv_screen_active(), .list = scroll.list };
    lv_display_t *disp = lv_display_get_default();
    int e = find(key_of(s.scr));
    slide_phase = 21;
    if (e < 0 || cache[e].dirty || !lv_obj_is_valid(s.list)) { scroll.queued = false; slide_phase = 0; return; }
    s.pic = cache[e].buf;
    s.strip = lv_draw_buf_create(DISP_W, 96, LV_COLOR_FORMAT_RGB565, 0);   // 87 KB; NULL: the slower path
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
    smp[0].t = scroll.t0;                                // the press and the 16 px that made it a scroll come first
    smp[0].p = ref_pos + (scroll.y1 - scroll.y0);        // (a flick can be over by now)
    smp[1].t = scroll.t1;
    smp[1].p = ref_pos;
    int ns = 2;
    bool down = true;
    int64_t t = esp_timer_get_time(), touched_at = t;
    for (;;) {
        int x, y, r = finger(&x, &y, &errs);
        int64_t now = esp_timer_get_time();
        if (down) {
            bool stuck = now - touched_at > FINGER_FOLLOW_US;
            if (stuck) ESP_LOGW(TAG, "scroll: finger down for 20 s, ending it");
            if (r == 0 || stuck) {                       // lifted (or 5 silent reads: the chip NACKs when idle)
                down = false;
                v = 0;                                   // speed over the last ~80 ms (0 if the finger stopped)
                if (ns) {                                // (none yet: touched again and lifted at once)
                    int nk = (ns - 1) % 8;
                    for (int i = ns - 2; i >= 0 && i >= ns - 8; i--) {
                        int k = i % 8;
                        int64_t span = smp[nk].t - smp[k].t;
                        if (span > 80000) break;
                        if (span >= 10000) v = (float)(smp[nk].p - smp[k].p) / (span / 1000.0f);
                    }
                    if (now - smp[nk].t > 80000) v = 0;
                }
                fpos = pos;
                t = now;
                if (stuck) break;
                continue;
            }
            if (r > 0) fy = y;                           // (a read error: keep the last point)
            int raw = ref_pos + (ref_y - fy);            // the content follows the finger
            if (!ns || now - smp[(ns - 1) % 8].t >= 8000) { smp[ns % 8].t = now; smp[ns % 8].p = raw; ns++; }
            int want = raw < lo ? lo - (lo - raw) / 3 : raw > hi ? hi + (raw - hi) / 3 : raw;   // resists past an end
            if (want < lo - over) want = lo - over;
            if (want > hi + over) want = hi + over;
            if (want == s.shown) { vTaskDelay(1); continue; }
            moved += abs(want - pos);
            pos = want;
            scroll_step(&s, pos);
            continue;
        }
        if (r > 0) {                                     // touched while moving: stop, follow the finger again
            down = true;
            touched_at = now;
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
            if (!past) break;
            // ...then spring back to the end (ease out, 220 ms)
            int from = pos, to = pos < lo ? lo : hi;
            int64_t start = esp_timer_get_time();
            for (;;) {
                float k = (esp_timer_get_time() - start) / 220000.0f;
                if (k > 1) k = 1;
                float ease = 1 - (1 - k) * (1 - k) * (1 - k);
                pos = from + (int)((to - from) * ease);
                scroll_step(&s, pos);
                if (k >= 1) break;
            }
            break;
        }
        fpos += v * dt;
        if (fpos < lo - over) { fpos = lo - over; v = 0; }
        if (fpos > hi + over) { fpos = hi + over; v = 0; }
        if ((int)fpos == pos) { vTaskDelay(1); continue; }
        moved += abs((int)fpos - pos);
        pos = (int)fpos;
        scroll_step(&s, pos);
    }
    painting = false;
    disp->inv_p = inv0;                                  // LVGL's redraw requests for the list: already on the panel
    if (s.strip) lv_draw_buf_destroy(s.strip);
    last_change = esp_timer_get_time();
    touch_resync(true);
    int64_t t1 = esp_timer_get_time();
    int f = s.frames ? s.frames : 1;
    ESP_LOGI(TAG, "scroll: %d frames in %lld ms, moved %d px, now at %d of %d..%d; per frame: move %.1f, render %.1f, "
             "send %.1f ms", s.frames, (t1 - t0) / 1000, moved, pos, lo, hi, s.us_move / 1000.0f / f,
             s.us_render / 1000.0f / f, s.us_send / 1000.0f / f);
    scroll.queued = false;
    slide_phase = 0;
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
    lv_draw_buf_t *strip = lv_draw_buf_create(DISP_W, 32, LV_COLOR_FORMAT_RGB565, 0);
    if (!strip) return -2;
    lv_display_t *disp = lv_display_get_default();
    uint32_t inv0 = disp->inv_p;
    painting = true;
    int bad = 0;
    for (int y0 = 0; y0 < DISP_H; y0 += 32) {
        int y1 = y0 + 31 > DISP_H - 1 ? DISP_H - 1 : y0 + 31;
        render_rows(lv_screen_active(), strip, y0, y0, y1);
        for (int y = y0; y <= y1; y++) {
            if (!memcmp(strip->data + (y - y0) * strip->header.stride, cache[e].buf->data + y * cache[e].buf->header.stride,
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

// The visible children of scr but img, with their alpha: ARGB8888 strips on a transparent background (~90 ms for
// the radar, mostly its full-screen range ring; taller strips didn't make it faster)
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
        lv_area_t rows = {0, y0, DISP_W - 1, y1}, in_buf = {0, 0, DISP_W - 1, y1 - y0};
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
            const uint8_t *p = strip->data + (y - y0) * strip->header.stride;   // B, G, R, A
            for (int x = 0; x < DISP_W; x++, p += 4) {
                if (!p[3] || ovl_n == OVL_CAP) continue;
                ovl_c[ovl_n] = rgb565_of(p[2], p[1], p[0]);
                ovl[ovl_n++] = (uint32_t)(y * DISP_W + x) | (uint32_t)p[3] << 24;
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
