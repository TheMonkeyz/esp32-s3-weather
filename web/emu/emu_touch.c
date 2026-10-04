// The finger, in the browser: the mouse or a touch on the canvas (emu.js calls emu_touch). Implements main/touch.h.
#include <emscripten.h>
#include "touch.h"
#include "esp_timer.h"

static volatile int down, tx, ty;
// A press LVGL hasn't read yet (a click can be over before it reads). Set by a new press only, not by moves: a drag
// (slide.c) reads the finger itself, and a flag left over from its moves made LVGL read one more press and release
// after it: a tap, which closed the hourly view after every day swipe. touch_forget() (after each drag) clears it.
static volatile int unseen;
static uint32_t forgotten;
static int64_t last_down;
static touch_read_hook_t hook;

EMSCRIPTEN_KEEPALIVE void emu_touch(int d, int x, int y)
{
    if (d && !down) unseen = 1;
    down = d;
    tx = x < 0 ? 0 : x > 465 ? 465 : x;
    ty = y < 0 ? 0 : y > 465 ? 465 : y;
    if (d) last_down = esp_timer_get_time();
}

static void read_cb(lv_indev_t *in, lv_indev_data_t *data)
{
    data->point.x = tx;
    data->point.y = ty;
    data->state = down || unseen ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    unseen = 0;
    if (hook) hook(in, data);                            // drags are recognised here (ui.c), before LVGL
}

void touch_init(void) {}
void touch_register_lvgl(void)
{
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, read_cb);
}
i2c_master_bus_handle_t touch_i2c_bus(void) { return NULL; }
void touch_inject(bool d, int x, int y) { emu_touch(d, x, y); }
void touch_inject_end(void) {}
bool touch_fresh(void) { return true; }                 // the mouse never lies
int touch_get(int *x, int *y) { *x = tx; *y = ty; return down; }
void touch_forget(void) { forgotten++; unseen = 0; }
uint32_t touch_forgotten(void) { return forgotten; }
uint32_t touch_idle_ms(void) { return down ? 0 : (uint32_t)((esp_timer_get_time() - last_down) / 1000); }
void touch_set_read_hook(touch_read_hook_t h) { hook = h; }
