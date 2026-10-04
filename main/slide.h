#pragma once
#include <stdbool.h>
#include "lvgl.h"

// Screen changes and drags drawn as sliding pictures. LVGL redraws every widget for every frame of a move (65-85 ms a
// frame: 3 or 4 frames in a 280 ms swipe), and even two LVGL images cost ~35 ms a frame (per-band object overhead).
// Here the screens are rendered into RGB565 pictures in PSRAM (~50-130 ms each), then each frame goes to the panel
// directly (display_raw_frame: rows copied from the pictures, ~15 ms, LVGL paused), and the real screen is shown
// again at the end. Pictures a drag needs are kept ready in a cache, rendered while idle (no delay when the finger
// starts). PSRAM short (room_for() in slide.c): plain lv_screen_load_anim / an unanimated switch.
// All functions: display lock held (LVGL task or caller); slides and drags run on the next LVGL cycle (lv_async_call).

// Same arguments as lv_screen_load_anim() (MOVE_LEFT/RIGHT/TOP/BOTTOM; anything else: lv_screen_load_anim()).
void slide_screen(lv_obj_t *to, lv_screen_load_anim_t how, uint32_t ms);
bool slide_running(void);          // a slide is waiting to start

// A page change decided elsewhere (the settings page picked another place): page `from`'s picture slides away and
// `to`'s comes in (dir +1: from below / the right, -1: from above / the left), then done(user) moves the pager there
// without an animation. Pictures come from the cache (rendered now if missing). False if a move is already running.
typedef void (*slide_done_cb_t)(void *user);
bool slide_page(const void *from, const void *to, bool vertical, int dir, slide_done_cb_t done, void *user);

// A drag that follows the finger (read from the touch chip directly while LVGL is paused), from the press point
// x0,y0, like LVGL's elastic scrolling: the neighbour on each side comes in under the finger; with no neighbour the
// screen resists and bounces back. On release: on to the neighbour past a third of the screen or after a flick,
// else back. neighbour(side): cache key of the neighbour on side -1 (left / above) or +1 (right / below): a screen
// object or a pager page; NULL at an end. commit(side): switch to that neighbour (load its screen / switch the page).
// Returns false (nothing happens) while a slide or drag is running: then let LVGL handle the touch. Returns true: the
// caller stops LVGL from also handling the touch (lv_indev_wait_release).
typedef const void *(*slide_neighbour_cb_t)(int side, void *user);
typedef void (*slide_commit_cb_t)(int side, void *user);
// x0,y0: where the finger went down; x1,y1: where it was when the move was recognised as a drag.
bool slide_drag(bool vertical, int x0, int y0, int x1, int y1, slide_neighbour_cb_t neighbour,
                slide_commit_cb_t commit, void *user);
bool slide_drag_running(void);
// Someone is using the screen: a finger down in the last 0.5 s, or a slide or drag queued or running. Any task (no
// lock): radar.c holds its flash writes meanwhile (they stall both cores, and a drag then crawled at 3 fps).
bool slide_screen_busy(void);
// When a drag, scroll or zoom last saw a finger (esp_timer us): LVGL doesn't see those touches
int64_t slide_last_touch(void);

// A zoom of a full-screen image (the radar's map) drawn without LVGL: its source (RGB565, the screen's size) scaled
// from `from` to `to` (LVGL's scale: 256 = 1x; both at least 1x; pivot = the centre) with an ease out over ms, the
// screen's other objects on top as they are; then LVGL's own scale is set to `to`. Runs on the next LVGL cycle.
// false: not now (another move, not the screen shown, a scale under 1x): animate with LVGL instead. LVGL doesn't see
// the touch meanwhile: a swipe made during the zoom is handed to swipe(dx, dy) at the end (LVGL task), or NULL.
typedef void (*slide_swipe_cb_t)(int dx, int dy);
bool slide_zoom(lv_obj_t *img, const uint16_t *src, int32_t from, int32_t to, uint32_t ms, slide_swipe_cb_t swipe);

// Picture cache: 5 pictures keyed by screen or pager page. paint(key, dst) renders the picture of key into dst (ui.c refreshes the
// screen's content first). The current screen's picture goes dirty when LVGL redraws anything on it; the others are
// marked dirty by the code that changes them (slide_cache_dirty, NULL = all). keep = the keys worth holding now (the
// current screen and its neighbours); slide_cache_idle_work() renders the first missing or dirty one, at most one
// per call, when nothing has changed on screen for quiet_ms (returns true if it rendered).
// key(screen): the cache key of what a screen shows now (a screen object, or the current page of a pager on it).
// paint(key, dst, y0, y1): rows y0..y1 of the picture (slide_picture_rows); background pictures are rendered a strip
// at a time so LVGL keeps reading the touch in between.
typedef bool (*slide_paint_cb_t)(const void *key, lv_draw_buf_t *dst, int y0, int y1);
typedef const void *(*slide_key_cb_t)(lv_obj_t *screen);
void slide_cache_init(slide_paint_cb_t paint, slide_key_cb_t key);
void slide_cache_keep(const void *const *keys, int n);
void slide_cache_dirty(const void *key);
void slide_cache_dirty_rows(const void *key, int y0, int y1);   // only rows y0..y1 changed (a clock): only they re-render
// Every picture but the screen shown's (its changes reach it as LVGL draws them) and those `except` returns true for
// (their owner marks the rows that change: the places' clocks, the radar's pill)
void slide_cache_dirty_hidden(bool (*except)(const void *key));
bool slide_cache_idle_work(int quiet_ms);
lv_draw_buf_t *slide_cache_get(const void *key, bool render);   // a clean picture, rendered now if `render`
bool slide_picture(lv_obj_t *scr, lv_draw_buf_t *dst);          // a whole screen into dst
bool slide_picture_rows(lv_obj_t *scr, lv_draw_buf_t *dst, int y0, int y1);   // only rows y0..y1 (paint callbacks)

// List scrolling drawn by moving the picture of the screen shown (see slide.c): target = the scrollable object under
// x,y (NULL: none); slide_scroll() takes over the touch on the next LVGL cycle: y0, t0 = the press (esp_timer us),
// y1 = where the drag was recognised (the flick speed starts from these). false: nothing happens (busy, or no exact
// picture of the screen yet): let LVGL scroll.
lv_obj_t *slide_scroll_target(lv_obj_t *scr, int x, int y);
bool slide_scroll(lv_obj_t *list, int y0, int64_t t0, int y1);
// A touch that stops a coasting list and then moves sideways (16 px, larger axis) is handed to this callback with
// where it went down and where it is: e.g. the hourly view's day drag. Returns true if it took the touch.
typedef bool (*slide_sideways_cb_t)(int x0, int y0, int x1, int y1);
void slide_scroll_on_sideways(slide_sideways_cb_t cb);
lv_draw_buf_t *slide_picture_copy(void);   // snapshot "picture" (tests)
void slide_picture_check_async(void);  // test console "pictest": logs "slide: pictest rows_differ=N first=Y"
