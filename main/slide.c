// Screen changes and drags as sliding pictures (see slide.h)
#include "slide.h"
#include <math.h>
#include <stdlib.h>
#include "display.h"
#include "touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
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
bool slide_picture_rows(lv_obj_t *scr, lv_draw_buf_t *dst, int y0, int y1)
{
    lv_obj_update_layout(scr);
    lv_area_t rows = {0, y0, DISP_W - 1, y1};
    lv_draw_buf_clear(dst, &rows);
    lv_layer_t layer;
    lv_memzero(&layer, sizeof(layer));
    layer.draw_buf = dst;
    layer.buf_area = (lv_area_t){0, 0, DISP_W - 1, DISP_H - 1};
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
    bool dirty;                          // out of date, or not finished
    int64_t dirty_since;
    int rows;                            // background rendering: rows done so far (0 = not started)
} cache[CACHE_N];
static const void *keep[CACHE_N];
static int keep_n;
static slide_paint_cb_t paint_cb;
static slide_key_cb_t key_cb;
static bool painting;                    // a page's picture: its pager moves there and back, not a change
static bool committing;                  // a drag's switch: the pager scrolls to the new page, no content changes
static int64_t last_change;              // last invalidation of the screen shown
static const void *just_loaded;          // screen shown by a slide: its first repaint is that same picture

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
    cache[e].key = key;
    cache[e].rows = 0;
    if (!paint_rows(e, 0, DISP_H - 1)) { cache[e].key = NULL; return NULL; }
    cache[e].dirty = false;
    return cache[e].buf;
}

lv_draw_buf_t *slide_cache_get(const void *key, bool render) { return get(key, render, false, NULL); }


void slide_cache_dirty(const void *key)
{
    for (int i = 0; i < CACHE_N; i++) {
        if ((key && cache[i].key != key) || cache[i].dirty) continue;
        cache[i].dirty = true;
        cache[i].dirty_since = esp_timer_get_time();
        cache[i].rows = 0;
    }
    for (int i = 0; i < CACHE_N; i++) if (!key || cache[i].key == key) cache[i].rows = 0;   // restart one in progress
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
        // its ages every second: a picture a second old is fine for a slide, the real screen comes back after it)
        if (e < 0 ? quiet : cache[e].dirty && (quiet || now - cache[e].dirty_since > 2000000)) {
            if (e < 0) {                                 // a slot for it: free, or holding a picture not needed now
                if ((e = slot_for(false, NULL, NULL)) < 0) return false;
                if (!cache[e].buf) {
                    if (!room_for(1)) return false;
                    cache[e].buf = lv_draw_buf_create(DISP_W, DISP_H, LV_COLOR_FORMAT_RGB565, 0);
                    if (!cache[e].buf) return false;
                }
                cache[e].key = keep[i];
                cache[e].dirty = true;
                cache[e].dirty_since = now;
                cache[e].rows = 0;
            }
            // One strip at a time, so LVGL reads the touch in between (dirtied meanwhile: starts again)
            int y0 = cache[e].rows, y1 = y0 + STRIP - 1 > DISP_H - 1 ? DISP_H - 1 : y0 + STRIP - 1;
            if (!paint_rows(e, y0, y1)) { cache[e].key = NULL; return false; }
            cache[e].rows = y1 + 1;
            if (cache[e].rows >= DISP_H) { cache[e].dirty = false; cache[e].rows = 0; }
            return true;
        }
    }
    return false;
}

// The screen shown changed (any redraw): its picture is out of date
static const void *key_of(lv_obj_t *scr) { return key_cb ? key_cb(scr) : scr; }

static void invalidated(lv_event_t *e)
{
    if (painting) return;
    if (committing) { last_change = esp_timer_get_time(); return; }
    const void *k = key_of(lv_screen_active());
    last_change = esp_timer_get_time();
    if (k != just_loaded) slide_cache_dirty(k);
}

static void rendered(lv_event_t *e) { just_loaded = NULL; }

void slide_cache_init(slide_paint_cb_t paint, slide_key_cb_t key)
{
    paint_cb = paint;
    key_cb = key;
    lv_display_t *d = lv_display_get_default();
    lv_display_add_event_cb(d, invalidated, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(d, rendered, LV_EVENT_RENDER_READY, NULL);
}

// After a slide: the real screen goes back up; LVGL repaints all of it, but it's the picture just shown
static void shown(lv_obj_t *scr)
{
    just_loaded = key_of(scr);
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

static bool req_pending(void) { return req.queued || drag.queued; }

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
        int x = drag.x0, y = drag.y0, nx, ny, r;
        while ((r = touch_get(&nx, &ny)) != 0) { if (r > 0) { x = nx; y = ny; } vTaskDelay(pdMS_TO_TICKS(10)); }
        int raw = drag.vertical ? y - drag.y0 : x - drag.x0;
        if (abs(raw) > S / 4 && drag.commit) drag.commit(raw > 0 ? -1 : 1, drag.user);   // commit ignores an end
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
        int nx, ny, r = touch_get(&nx, &ny);
        // Finger up. The chip often stops answering when nothing touches it (NACK) instead of reporting a release:
        // 5 silent reads in a row (~75 ms) are a release too, as in touch.c (else the drag never ended)
        if (r == 0 || (r < 0 && ++errs >= 5)) break;
        if (r > 0) { x = nx; y = ny; errs = 0; samples++; }
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
        just_loaded = nkey[si];                              // its picture is what the panel shows now
        // The switch scrolls the pager (SCROLL_BEGIN, layout): LVGL redraws the page being left, whose content didn't
        // change. Counted as a change, its picture went out of date at every switch, and a drag back soon after
        // waited ~0.2 s for it. Content changes made meanwhile still mark their pictures (slide_cache_dirty).
        committing = true;
        drag.commit(side, drag.user);                        // loads the new screen / switches the page
        committing = false;
        lv_obj_invalidate(lv_screen_active());
    } else {
        just_loaded = ckey;
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
