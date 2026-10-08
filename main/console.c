// The display's own test console commands and diagnostics lines, registered with espforge's forge_core (testcon.h,
// diag.h): forge_core's built-ins are ping, help, heap, where, memspeed, reboot; forge_net adds wifi, key, portal.
// Every answer is a log line starting with "test: " (docs/TESTING.md, "Harness"). Moved from testcon.c and diag.c,
// October 5 (v1.14.0).
#include "console.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "testcon.h"
#include "diag.h"
#include "display.h"
#include "slide.h"
#include "touch.h"
#include "ui.h"
#include "config.h"

static const char *TAG = "test";

#define CX 233
#define CY 233

// Finger moves in steps of ~16 ms (LVGL reads the touch every 30 ms, so every read sees a new point)
static void finger_path(int x0, int y0, int x1, int y1, int ms)
{
    int steps = ms / 16 < 2 ? 2 : ms / 16;
    for (int i = 0; i <= steps; i++) {
        touch_inject(true, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

static void finger_up(int x, int y)
{
    touch_inject(false, x, y);
    vTaskDelay(pdMS_TO_TICKS(100));          // LVGL must read the release before the real controller takes over
    touch_inject_end();
}

static void press(int x, int y, int ms)
{
    touch_inject(true, x, y);
    vTaskDelay(pdMS_TO_TICKS(ms));
    finger_up(x, y);
}

static int ms_arg(const char *s)                          // 0..10000 ms
{
    int v = atoi(s);
    return v < 0 ? 0 : v > 10000 ? 10000 : v;
}

/* ---------- commands ---------- */

static void cmd_screen(int argc, char **argv)
{
    if (!display_lock(2000)) {                           // the console must stay free for "where"
        ESP_LOGW(TAG, "error screen: display busy for 2 s (send 'where')");
        return;
    }
    const char *n = ui_screen_name();
    display_unlock();
    ESP_LOGI(TAG, "screen %s", n);
}

static void cmd_page(int argc, char **argv)
{
    if (!display_lock(2000)) { ESP_LOGW(TAG, "error page: display busy for 2 s (send 'where')"); return; }
    int place, day, places, days;
    ui_pages(&place, &day, &places, &days);
    display_unlock();
    ESP_LOGI(TAG, "page place=%d places=%d day=%d days=%d", place, places, day, days);
}

static void cmd_tap(int argc, char **argv)
{
    if (argc != 3) { ESP_LOGW(TAG, "error tap X Y"); return; }
    press(atoi(argv[1]), atoi(argv[2]), 120);   // a real tap is ~80-150 ms; 60 ms fitted inside one full redraw
    ESP_LOGI(TAG, "ok tap");
}

static void cmd_press(int argc, char **argv)            // long-press: press X Y [ms, default 1200]
{
    if (argc < 3) { ESP_LOGW(TAG, "error press X Y [ms]"); return; }
    press(atoi(argv[1]), atoi(argv[2]), argc > 3 ? ms_arg(argv[3]) : 1200);
    ESP_LOGI(TAG, "ok press");
}

static void cmd_swipe(int argc, char **argv)
{
    int d = 150, x0 = CX, y0 = CY, x1 = CX, y1 = CY;
    const char *dir = argc == 2 ? argv[1] : "";
    if (!strcmp(dir, "left"))       { x0 = CX + d; x1 = CX - d; }
    else if (!strcmp(dir, "right")) { x0 = CX - d; x1 = CX + d; }
    else if (!strcmp(dir, "up"))    { y0 = CY + d; y1 = CY - d; }
    else if (!strcmp(dir, "down"))  { y0 = CY - d; y1 = CY + d; }
    else { ESP_LOGW(TAG, "error swipe: left, right, up or down"); return; }
    finger_path(x0, y0, x1, y1, 200);
    finger_up(x1, y1);
    ESP_LOGI(TAG, "ok swipe %s", dir);
}

static void cmd_drag(int argc, char **argv)             // drag X1 Y1 X2 Y2 [ms]
{
    if (argc < 5) { ESP_LOGW(TAG, "error drag X1 Y1 X2 Y2 [ms]"); return; }
    int x1 = atoi(argv[3]), y1 = atoi(argv[4]);
    finger_path(atoi(argv[1]), atoi(argv[2]), x1, y1, argc > 5 ? ms_arg(argv[5]) : 400);
    finger_up(x1, y1);
    ESP_LOGI(TAG, "ok drag");
}

static void cmd_fps(int argc, char **argv)              // "fps reset", then "fps": frames since the reset
{
    display_stats_t d;
    display_get_test_stats(&d, true);
    if (argc == 2 && !strcmp(argv[1], "reset")) { ESP_LOGI(TAG, "ok fps reset"); return; }
    ESP_LOGI(TAG, "fps frames=%lu render_avg_ms=%.1f render_max_ms=%.1f anim_frames=%lu anim_fps=%.1f "
                  "gap_max_ms=%.1f gap_max_at_ms=%lu gap_max_kind=%s mpx=%.2f", (unsigned long)d.frames,
             d.frames ? d.render_us / 1000.0f / d.frames : 0, d.render_max_us / 1000.0f,
             (unsigned long)d.anim_frames, d.anim_us ? d.anim_frames * 1e6f / d.anim_us : 0,
             d.anim_gap_max_us / 1000.0f, (unsigned long)d.anim_gap_max_at_ms,
             d.anim_gap_max_kind[0] ? d.anim_gap_max_kind : "-", d.pixels / 1e6f);
}

#if LV_USE_PROFILER && LV_USE_PROFILER_BUILTIN
// "profile": one render-only frame of each screen with LVGL's profiler on; the trace lines go to the log and
// tools/harness/profile.py adds them up. Needs CONFIG_LV_USE_PROFILER(_BUILTIN) in the build's sdkconfig.
#include "misc/lv_profiler_builtin_private.h"
static uint32_t prof_tick(void) { return (uint32_t)esp_timer_get_time(); }
static void prof_flush(const char *buf) { printf("%s", buf); }
static void profile_task(void *arg)
{
    lv_profiler_builtin_uninit();
    lv_profiler_builtin_config_t cfg;
    lv_profiler_builtin_config_init(&cfg);
    cfg.buf_size = 512 * 1024;
    cfg.tick_per_sec = 1000000;
    cfg.tick_get_cb = prof_tick;
    cfg.flush_cb = prof_flush;
    lv_profiler_builtin_init(&cfg);
    lv_profiler_builtin_set_enable(false);
    lv_obj_t *scr[6];
    const char *name[6];
    display_lock(-1);
    int n = ui_bench_screens(scr, name, 6);
    display_bench_no_panel(true);
    for (int i = 0; i < n; i++) {
        lv_screen_load(scr[i]);
        lv_refr_now(NULL);                               // warm caches
        lv_profiler_builtin_set_enable(true);
        lv_obj_invalidate(scr[i]);
        lv_refr_now(NULL);
        lv_profiler_builtin_set_enable(false);
        printf("PROFILE-BEGIN %s\n", name[i]);
        lv_profiler_builtin_flush();
        printf("PROFILE-END %s\n", name[i]);
    }
    lv_screen_load(ui_main_screen());
    display_bench_no_panel(false);
    lv_obj_invalidate(lv_screen_active());
    display_unlock();
    ESP_LOGI(TAG, "ok profile done");
    vTaskDelete(NULL);
}

static void cmd_profile(int argc, char **argv)          // one frame of each screen (docs/TESTING.md §8)
{
    xTaskCreatePinnedToCore(profile_task, "profile", 10240, NULL, 4, NULL, 1);
}
#endif

static void cmd_pictest(int argc, char **argv)          // the picture of the screen shown = the screen? (slide.c)
{
    if (!display_lock(2000)) { ESP_LOGW(TAG, "error pictest: display busy for 2 s (send 'where')"); return; }
    slide_picture_check_async();
    display_unlock();
    ESP_LOGI(TAG, "ok pictest (result: \"slide: pictest\" line)");
}

static void (*alert_at_cb)(bool on, double lat, double lon);
void console_on_alert_at(void (*fn)(bool on, double lat, double lon)) { alert_at_cb = fn; }

static void cmd_alert(int argc, char **argv)            // the alert screen's layout; alerts from another point
{
    if (argc >= 3 && !strcmp(argv[1], "at") && alert_at_cb) {
        bool off = argc == 3 && !strcmp(argv[2], "off");
        if (!off && argc != 4) { ESP_LOGW(TAG, "error alert at LAT LON|off"); return; }
        double lat = off ? 0 : atof(argv[2]), lon = off ? 0 : atof(argv[3]);
        alert_at_cb(!off, lat, lon);
        if (off) ESP_LOGI(TAG, "ok alert at off");
        else ESP_LOGI(TAG, "ok alert at %.4f %.4f (the first place's alerts, until 'alert at off' or a restart)", lat, lon);
        return;
    }
    if (argc != 3 || strcmp(argv[1], "sample")) { ESP_LOGW(TAG, "error alert sample en|fr|max|off, alert at LAT LON|off"); return; }
    if (!display_lock(2000)) { ESP_LOGW(TAG, "error alert: display busy for 2 s (send 'where')"); return; }
    int th, lines, by;
    bool ok = ui_alert_sample(argv[2], &th, &lines, &by);
    display_unlock();
    if (!ok) { ESP_LOGW(TAG, "error alert sample: en, fr, max or off"); return; }
    ESP_LOGI(TAG, "alert sample %s title_y=40 title_h=%d lines=%d box_y=%d", argv[2], th, lines, by);
}

static void cmd_setup(int argc, char **argv)            // the Easy Connect page after a failed attempt
{
    if (argc != 2 || strcmp(argv[1], "fail")) { ESP_LOGW(TAG, "error setup fail"); return; }
    if (!display_lock(2000)) { ESP_LOGW(TAG, "error setup: display busy for 2 s (send 'where')"); return; }
    int lines, bottom;
    bool ok = ui_setup_fail_sample(&lines, &bottom);
    display_unlock();
    if (!ok) { ESP_LOGW(TAG, "error setup fail: only on the Easy Connect page"); return; }
    ESP_LOGI(TAG, "setup fail lines=%d bottom=%d", lines, bottom);
}

static void cmd_dirty(int argc, char **argv)            // as new data does: hidden pictures out of date, shown redrawn
{
    if (!display_lock(2000)) { ESP_LOGW(TAG, "error dirty: display busy for 2 s"); return; }
    slide_cache_dirty_hidden(NULL);
    lv_obj_invalidate(lv_screen_active());
    display_unlock();
    ESP_LOGI(TAG, "ok dirty");
}

// "memlow start", then "memlow stop": the lowest free PSRAM and internal RAM in between (ESP-IDF's local minimum; the
// since-boot minimums are then put back, as the lower of the two). Meanwhile "heap" and the diag lines' "min ever"
// are the window's. The harness's alert test reads PSRAM's low point over the test alone.
static void cmd_memlow(int argc, char **argv)
{
    static bool on;
    if (argc == 2 && !strcmp(argv[1], "start")) {
        if (on) heap_caps_monitor_local_minimum_free_size_stop();
        on = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
        if (on) ESP_LOGI(TAG, "ok memlow start");
        else ESP_LOGW(TAG, "error memlow: could not start");
        return;
    }
    if (argc != 2 || strcmp(argv[1], "stop")) { ESP_LOGW(TAG, "error memlow start|stop"); return; }
    if (!on) { ESP_LOGW(TAG, "error memlow: not started"); return; }
    unsigned psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024;
    unsigned internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024;
    heap_caps_monitor_local_minimum_free_size_stop();
    on = false;
    ESP_LOGI(TAG, "memlow psram_min=%u internal_min=%u", psram, internal);
}

static void cmd_hint(int argc, char **argv)             // first-run hint, places kept
{
    if (argc != 2 || strcmp(argv[1], "next-boot")) { ESP_LOGW(TAG, "error hint next-boot"); return; }
    config_hint_next_boot();
    ESP_LOGI(TAG, "ok hint next-boot");
}

// "memspeed" (replaces forge_core's): copy speeds that bound full-screen drawing (PSRAM <-> internal, CPU memcpy
// and LVGL's lv_memcpy)
static void cmd_memspeed(int argc, char **argv)
{
    const size_t big = 466 * 466 * 2, band = 466 * 16 * 2;
    uint8_t *ps = heap_caps_malloc(big, MALLOC_CAP_SPIRAM), *ps2 = heap_caps_malloc(big, MALLOC_CAP_SPIRAM);
    uint8_t *in = heap_caps_malloc(band, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!ps || !ps2 || !in) { ESP_LOGW(TAG, "error memspeed: no memory"); goto out; }
    memset(ps, 0x55, big); memset(ps2, 0x55, big);
    int64_t t0 = esp_timer_get_time();
    for (size_t o = 0; o + band <= big; o += band) memcpy(in, ps + o, band);           // PSRAM -> internal
    int64_t t1 = esp_timer_get_time();
    for (size_t o = 0; o + band <= big; o += band) memcpy(ps2 + o, in, band);          // internal -> PSRAM
    int64_t t2 = esp_timer_get_time();
    for (size_t o = 0; o + band <= big; o += band) lv_memcpy(in, ps + o, band);
    int64_t t3 = esp_timer_get_time();
    memcpy(ps2, ps, big);                                                               // PSRAM -> PSRAM
    int64_t t4 = esp_timer_get_time();
    ESP_LOGI(TAG, "memspeed screen=%u B: psram_to_internal_ms=%.1f internal_to_psram_ms=%.1f lv_memcpy_ms=%.1f "
                  "psram_to_psram_ms=%.1f", (unsigned)big, (t1 - t0) / 1000.0f, (t2 - t1) / 1000.0f,
             (t3 - t2) / 1000.0f, (t4 - t3) / 1000.0f);
out:
    free(ps); free(ps2); free(in);
}

/* ---------- render bench ("diag: bench" lines) ---------- */

// Full-screen render timing of each screen. Invisible: other screens are rendered without being sent to the panel;
// the panel transfer is timed by repainting the (unchanged) weather screen. Blocks the UI for about 1.5 s.
static bool bench(void)
{
    lv_obj_t *scr[6];
    const char *name[6];
    const int N = 5;
    display_lock(-1);
    lv_obj_t *was = lv_screen_active();
    lv_indev_t *in = lv_indev_get_next(NULL);
    // Only while the weather screen is idle: the last step repaints it on the panel, which would flash over any other
    // screen (seen once over the hourly view)
    if (was != ui_main_screen() || (in && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED) || lv_anim_count_running()) {
        display_unlock();
        ESP_LOGI("diag", "bench postponed (screen in use)");
        return false;
    }
    display_bench_no_panel(true);
    int n = ui_bench_screens(scr, name, 6);
    char line[160];
    int len = 0;
    for (int i = 0; i < n; i++) {
        lv_screen_load(scr[i]);
        lv_refr_now(NULL);                                          // warm the glyph cache
        int64_t t0 = esp_timer_get_time();
        for (int k = 0; k < N; k++) { lv_obj_invalidate(scr[i]); lv_refr_now(NULL); }
        len += snprintf(line + len, sizeof(line) - len, " %s %.1f ms |", name[i],
                        (esp_timer_get_time() - t0) / 1000.0f / N);
    }
    lv_screen_load(ui_main_screen());
    lv_refr_now(NULL);
    display_bench_no_panel(false);
    int64_t t0 = esp_timer_get_time();
    for (int k = 0; k < N; k++) { lv_obj_invalidate(lv_screen_active()); lv_refr_now(NULL); }
    float with_panel = (esp_timer_get_time() - t0) / 1000.0f / N;
    display_unlock();
    ESP_LOGI("diag", "bench render-only full screen:%s", line);
    ESP_LOGI("diag", "bench weather incl. panel transfer: %.1f ms/frame", with_panel);
    return true;
}

static void bench_task(void *arg)
{
    bench();
    vTaskDelete(NULL);
}

// In its own task: rendering needs the LVGL task's stack size, not the console's (4 KB overflowed and reset the board)
static void cmd_bench(int argc, char **argv)
{
    xTaskCreatePinnedToCore(bench_task, "bench", 10240, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "ok bench started (result: \"diag: bench\" lines)");
}

/* ---------- breadcrumbs and the periodic display line ---------- */

// "where" (no lock): what the moves and the display code are doing now
static void where_display(char *out, size_t n)
{
    extern volatile int slide_phase, raw_phase, raw_band;
    extern int display_lvgl_inflight(void);
    snprintf(out, n, " slide_phase=%d raw_phase=%d raw_band=%d lvgl_inflight=%d", slide_phase, raw_phase, raw_band,
             display_lvgl_inflight());
}

// After each "diag: heap" line: frame timing and lock contention since the last one
static void diag_display(void)
{
    display_stats_t d;
    display_get_stats(&d, true);
    ESP_LOGI("diag", "display: %lu frames, render avg %.1f ms max %.1f ms, %.1f Mpx sent | "
             "animation %.1f fps (%lu frames, worst gap %.0f ms) | LVGL lock wait max %.1f ms, "
             "longest hold %.1f ms by %s",
             (unsigned long)d.frames, d.frames ? d.render_us / 1000.0 / d.frames : 0, d.render_max_us / 1000.0,
             d.pixels / 1e6, d.anim_us ? d.anim_frames * 1e6 / d.anim_us : 0, (unsigned long)d.anim_frames,
             d.anim_gap_max_us / 1000.0, d.lvgl_wait_max_us / 1000.0, d.hold_max_us / 1000.0,
             d.hold_task[0] ? d.hold_task : "-");
}

void console_init(void)
{
    testcon_register("screen", "screen", cmd_screen);
    testcon_register("page", "page", cmd_page);
    testcon_register("tap", "tap X Y", cmd_tap);
    testcon_register("press", "press X Y [ms]", cmd_press);
    testcon_register("swipe", "swipe left|right|up|down", cmd_swipe);
    testcon_register("drag", "drag X1 Y1 X2 Y2 [ms]", cmd_drag);
    // (presence [calibrate N] and wake: forge_presence's, registered by presence_start())
    testcon_register("fps", "fps [reset]", cmd_fps);
#if LV_USE_PROFILER && LV_USE_PROFILER_BUILTIN
    testcon_register("profile", "profile", cmd_profile);
#endif
    testcon_register("pictest", "pictest", cmd_pictest);
    testcon_register("alert", "alert sample en|fr|max|off, alert at LAT LON|off", cmd_alert);
    testcon_register("setup", "setup fail", cmd_setup);
    testcon_register("memlow", "memlow start|stop", cmd_memlow);
    testcon_register("dirty", "dirty", cmd_dirty);
    testcon_register("hint", "hint next-boot", cmd_hint);
    testcon_register("bench", "bench", cmd_bench);
    testcon_register("memspeed", "memspeed", cmd_memspeed);    // after testcon_start(): replaces the built-in
    testcon_add_where(where_display);
    diag_add_hook(diag_display);
}
