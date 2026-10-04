// LVGL 8 vs 9 render benchmark for this board (docs/IDEAS.md "LVGL 8 instead of 9"). The same four screens, built like
// the app's (Montserrat through Tiny TTF without kerning, 466x466 RGB565, 32-row partial buffers in internal DMA RAM),
// are redrawn whole 20 times each; the flush returns at once, so only LVGL's own rendering is timed (the panel transfer
// is the same for both). Built twice: tools/lvglbench/v8 and v9 (README.md). Output: "bench: <screen> median N ms ...".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#define W 466
#define H 466
#define BUF_LINES 32
#define REPS 20
static const char *TAG = "bench";

extern const uint8_t ttf_start[] asm("_binary_montserrat_ttf_start");
extern const uint8_t ttf_end[]   asm("_binary_montserrat_ttf_end");
#if LVGL_VERSION_MAJOR >= 9
extern const uint8_t syl_start[] asm("_binary_syllabics_ttf_start");
extern const uint8_t syl_end[]   asm("_binary_syllabics_ttf_end");
#endif

#if LVGL_VERSION_MAJOR >= 9
static lv_display_t *disp;
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) { lv_display_flush_ready(d); }
static lv_font_t *mkfont(int px)
{
    return lv_tiny_ttf_create_data_ex(ttf_start, ttf_end - ttf_start, px, LV_FONT_KERNING_NONE, 96);
}
#define SCR_ACT() lv_screen_active()
#define IMG_CREATE lv_image_create
#define IMG_SET_SRC lv_image_set_src
typedef lv_image_dsc_t img_dsc_t;
typedef lv_point_precise_t pt_t;
#else
static lv_disp_t *disp;
static lv_disp_draw_buf_t dbuf;
static lv_disp_drv_t drv;
static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) { lv_disp_flush_ready(d); }
static lv_font_t *mkfont(int px)
{
    // (v8's Tiny TTF always kerns and looks each glyph's metrics up on every draw: no switch as v9's)
    // (and its cache size is in bytes, v9's in glyphs: 96 glyphs of px x px, as the app's 96)
    return lv_tiny_ttf_create_data_ex(ttf_start, ttf_end - ttf_start, px, (size_t)96 * px * px);
}
#define SCR_ACT() lv_scr_act()
#define IMG_CREATE lv_img_create
#define IMG_SET_SRC lv_img_set_src
typedef lv_img_dsc_t img_dsc_t;
typedef lv_point_t pt_t;
#endif

static const lv_font_t *f_big, *f_time, *f_small;
static const char *fonts = "ttf";
static bool wide;                                // labels 400 px wide, text centred (as the app's label())
static lv_obj_t *scr[4];
static const char *names[4] = {"blank", "weather", "hourly", "radar"};

static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *t, lv_align_t al, int x, int y)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, t);
    if (wide && al == LV_ALIGN_TOP_MID) {
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, 400);
    }
    lv_obj_align(l, al, x, y);
    return l;
}

static lv_obj_t *blob(lv_obj_t *p, int d, lv_color_t c, int x, int y)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
    return o;
}

static lv_obj_t *screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static void build(void)
{
    for (int k = 0; k < 4; k++) if (scr[k]) lv_obj_del(scr[k]);
    lv_color_t white = lv_color_white(), grey = lv_color_make(0x8a, 0x8f, 0x98), blue = lv_color_make(0x5a, 0xb0, 0xff),
               sun = lv_color_make(0xf7, 0xc8, 0x3d), cloud = lv_color_make(0xc8, 0xd0, 0xd8);
    scr[0] = screen();

    // weather: like the app's main screen (clock, city, big temperature, sun, condition, details, three days)
    lv_obj_t *s = scr[1] = screen();
    label(s, f_time, grey, "13:37", LV_ALIGN_TOP_MID, 0, 30);
    label(s, f_time, white, "Quebec", LV_ALIGN_TOP_MID, 0, 68);
    blob(s, 64, sun, 120, 128);
    label(s, f_big, white, "14\xC2\xB0", LV_ALIGN_TOP_LEFT, 215, 108);
    label(s, f_time, white, "Clear sky", LV_ALIGN_TOP_MID, 0, 215);
    label(s, f_small, grey, "Feels 11\xC2\xB0 \xC2\xB7 49% \xC2\xB7 14 km/h", LV_ALIGN_TOP_MID, 0, 248);
    lv_obj_t *rule = lv_obj_create(s);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 260, 1);
    lv_obj_set_style_bg_color(rule, grey, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_50, 0);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 297);
    static const char *days[3] = {"Today", "Sun", "Mon"};
    for (int i = 0; i < 3; i++) {
        int x = 103 + i * 98;
        label(s, f_small, blue, days[i], LV_ALIGN_TOP_LEFT, x - 25, 308);
        blob(s, 34, cloud, x - 10, 338);
        blob(s, 22, sun, x + 4, 332);
        label(s, f_small, white, "15\xC2\xB0 / 5\xC2\xB0", LV_ALIGN_TOP_LEFT, x - 30, 382);
    }

    // hourly: title, a line graph over vertical hour lines, four rows (one highlighted, translucent)
    s = scr[2] = screen();
    label(s, f_time, blue, "Saturday", LV_ALIGN_TOP_MID, 0, 34);
    label(s, f_small, grey, "Partly cloudy \xC2\xB7 15\xC2\xB0 / 5\xC2\xB0", LV_ALIGN_TOP_MID, 0, 68);
    static pt_t pts[25];
    for (int i = 0; i < 25; i++) { pts[i].x = 103 + i * 11; pts[i].y = 150 - (i * 37 % 41); }
    for (int i = 0; i < 25; i++) {
        lv_obj_t *v = lv_obj_create(s);
        lv_obj_remove_style_all(v);
        lv_obj_set_size(v, 1, 60);
        lv_obj_set_style_bg_color(v, grey, 0);
        lv_obj_set_style_bg_opa(v, LV_OPA_30, 0);
        lv_obj_align(v, LV_ALIGN_TOP_LEFT, 103 + i * 11, 118);
    }
    lv_obj_t *ln = lv_line_create(s);
    lv_line_set_points(ln, pts, 25);
    lv_obj_set_style_line_width(ln, 3, 0);
    lv_obj_set_style_line_color(ln, sun, 0);
    lv_obj_set_style_line_rounded(ln, true, 0);
    lv_obj_t *hl = lv_obj_create(s);
    lv_obj_remove_style_all(hl);
    lv_obj_set_size(hl, 316, 40);
    lv_obj_set_style_radius(hl, 12, 0);
    lv_obj_set_style_bg_color(hl, blue, 0);
    lv_obj_set_style_bg_opa(hl, LV_OPA_20, 0);
    lv_obj_align(hl, LV_ALIGN_TOP_MID, 0, 242);
    static const char *hrs[4] = {"Now", "14:00", "15:00", "16:00"};
    for (int i = 0; i < 4; i++) {
        int y = 250 + i * 46;
        label(s, f_small, i ? grey : blue, hrs[i], LV_ALIGN_TOP_LEFT, 86, y);
        blob(s, 24, sun, 160, y);
        label(s, f_small, white, "14\xC2\xB0", LV_ALIGN_TOP_LEFT, 222, y);
        label(s, f_small, grey, "0%   14 km/h", LV_ALIGN_TOP_LEFT, 280, y);
    }

    // radar: a full-screen RGB565 picture (a map stand-in), a translucent pill and labels, a range ring, a dot
    s = scr[3] = screen();
    static img_dsc_t img;
    uint16_t *px = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) px[y * W + x] = (uint16_t)(((x >> 3) & 31) << 11 | ((y >> 2) & 63) << 5 | ((x + y) >> 4 & 31));
    memset(&img, 0, sizeof(img));
#if LVGL_VERSION_MAJOR >= 9
    img.header.magic = LV_IMAGE_HEADER_MAGIC;
    img.header.cf = LV_COLOR_FORMAT_RGB565;
    img.header.stride = W * 2;
#else
    img.header.cf = LV_IMG_CF_TRUE_COLOR;
#endif
    img.header.w = W;
    img.header.h = H;
    img.data_size = W * H * 2;
    img.data = (const uint8_t *)px;
    lv_obj_t *im = IMG_CREATE(s);
    IMG_SET_SRC(im, &img);
    lv_obj_center(im);
    lv_obj_t *ring = lv_obj_create(s);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 238, 238);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, white, 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_30, 0);
    lv_obj_center(ring);
    lv_obj_t *pill = lv_obj_create(s);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 90, 34);
    lv_obj_set_style_radius(pill, 14, 0);
    lv_obj_set_style_bg_color(pill, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_60, 0);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 30);
    label(pill, f_time, white, "13:37", LV_ALIGN_CENTER, 0, 0);
    label(s, f_small, white, "Radar 13:30 \xC2\xB7 195 km", LV_ALIGN_TOP_MID, 0, 74);
    label(s, f_small, white, "100 km", LV_ALIGN_CENTER, 0, 131);
    blob(s, 12, blue, 227, 227);
}

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void bench_task(void *arg)
{
    lv_init();
    size_t bytes = W * BUF_LINES * 2;
    void *b1 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *b2 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#if LVGL_VERSION_MAJOR >= 9
    disp = lv_display_create(W, H);
    lv_display_set_flush_cb(disp, flush);
    lv_display_set_buffers(disp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
#else
    lv_disp_draw_buf_init(&dbuf, b1, b2, W * BUF_LINES);
    lv_disp_drv_init(&drv);
    drv.hor_res = W;
    drv.ver_res = H;
    drv.flush_cb = flush;
    drv.draw_buf = &dbuf;
    disp = lv_disp_drv_register(&drv);
#endif
    f_big = mkfont(96);
    f_time = mkfont(26);
    f_small = mkfont(20);
    build();
    ESP_LOGI(TAG, "LVGL %d.%d.%d, %d reps per screen, internal free %u KB", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH, REPS, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
#if LVGL_VERSION_MAJOR >= 9
    const int passes = 5;
#else
    const int passes = 2;
#endif
    for (int pass = 0; pass < passes; pass++) {
#if LVGL_VERSION_MAJOR >= 9
    if (pass >= 2) {                                     // the app's label and font details, one at a time
        lv_obj_t *keep = screen();
        lv_screen_load(keep);
        wide = pass == 2 || pass == 4;
        f_big = mkfont(96);
        f_time = mkfont(26);
        f_small = mkfont(20);
        if (pass >= 3) {                                 // a syllabics fallback behind every size (the app's mkfont)
            ((lv_font_t *)f_big)->fallback = lv_tiny_ttf_create_data_ex(syl_start, syl_end - syl_start, 120, LV_FONT_KERNING_NONE, 96);
            ((lv_font_t *)f_time)->fallback = lv_tiny_ttf_create_data_ex(syl_start, syl_end - syl_start, 32, LV_FONT_KERNING_NONE, 96);
            ((lv_font_t *)f_small)->fallback = lv_tiny_ttf_create_data_ex(syl_start, syl_end - syl_start, 25, LV_FONT_KERNING_NONE, 96);
        }
        fonts = pass == 2 ? "ttf+wide" : pass == 3 ? "ttf+fallback" : "ttf+wide+fallback";
        build();
    } else
#endif
    if (pass) {                                          // pass 2: pre-rendered fonts, the font engine out of the way
        lv_obj_t *keep = screen();
#if LVGL_VERSION_MAJOR >= 9
        lv_screen_load(keep);
#else
        lv_scr_load(keep);
#endif
        f_big = &lv_font_montserrat_48;
        f_time = &lv_font_montserrat_28;
        f_small = &lv_font_montserrat_20;
        fonts = "bitmap";
        build();
    }
    for (int round = 0; round < 2; round++) {            // round 0 warms the glyph caches; round 1 is reported
        for (int k = 0; k < 4; k++) {
#if LVGL_VERSION_MAJOR >= 9
            lv_screen_load(scr[k]);
#else
            lv_scr_load(scr[k]);
#endif
            lv_refr_now(disp);
            int ms[REPS];
            for (int i = 0; i < REPS; i++) {
                lv_obj_invalidate(SCR_ACT());
                int64_t t = esp_timer_get_time();
                lv_refr_now(disp);
                ms[i] = (int)((esp_timer_get_time() - t) / 100);   // 0.1 ms
                vTaskDelay(1);
            }
            qsort(ms, REPS, sizeof(int), cmp);
            if (round)
                ESP_LOGI(TAG, "%s %s median %d.%d ms, min %d.%d, max %d.%d", fonts, names[k], ms[REPS / 2] / 10, ms[REPS / 2] % 10,
                         ms[0] / 10, ms[0] % 10, ms[REPS - 1] / 10, ms[REPS - 1] % 10);
        }
    }
    }
    ESP_LOGI(TAG, "done");
    vTaskDelete(NULL);
}

void app_main(void)
{
    xTaskCreatePinnedToCore(bench_task, "bench", 16384, NULL, 4, NULL, 1);   // core 1, as the app's LVGL task
}
