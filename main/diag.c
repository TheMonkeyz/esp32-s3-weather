// Diagnostics logger (see diag.h)
#include "diag.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include "nvs.h"
#include "esp_app_desc.h"
#include "display.h"
#include "ui.h"

static const char *TAG = "diag";
bool diag_bench(void);
static volatile bool bench_done;
#define MAX_TASKS 32
#define BENCH_AT_S 0      // render benchmark this long after boot (0 = never). Was 45: it blocks LVGL ~1.5 s, which
                          // swallowed swipes a minute after boot; the harness runs it on demand ("bench").
#define KB(x) ((unsigned)((x) / 1024))

typedef struct { TaskHandle_t h; uint32_t rt; } prev_t;
static prev_t prev[MAX_TASKS];
static int nprev;
static uint32_t prev_total;

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT WDT";
    case ESP_RST_TASK_WDT: return "TASK WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB: return "usb";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other";
    }
}

static void startup_info(void)
{
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    ESP_LOGI(TAG, "firmware %s (built %s %s)", esp_app_get_description()->version,
             esp_app_get_description()->date, esp_app_get_description()->time);
    ESP_LOGI(TAG, "reset reason %s, flash %u MB, PSRAM %u KB", reset_reason(), (unsigned)(flash >> 20),
             KB(esp_psram_get_size()));
    const esp_partition_t *app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (app) ESP_LOGI(TAG, "app partition %u KB at 0x%lx", KB(app->size), (unsigned long)app->address);
    nvs_stats_t ns;
    if (nvs_get_stats(NULL, &ns) == ESP_OK)
        ESP_LOGI(TAG, "NVS entries: %u used, %u free of %u", (unsigned)ns.used_entries,
                 (unsigned)ns.free_entries, (unsigned)ns.total_entries);
}

static int cmp_rt(const void *a, const void *b)
{
    const TaskStatus_t *x = a, *y = b;                            // counters hold deltas here
    return (y->ulRunTimeCounter > x->ulRunTimeCounter) - (y->ulRunTimeCounter < x->ulRunTimeCounter);
}

static void report_tasks(void)
{
    static TaskStatus_t ts[MAX_TASKS];
    uint32_t total;
    int n = uxTaskGetSystemState(ts, MAX_TASKS, &total);
    uint32_t window = total - prev_total;                         // µs of wall time (per core)
    // Replace each counter by its delta since the last report
    prev_t now[MAX_TASKS];
    for (int i = 0; i < n; i++) {
        uint32_t rt = ts[i].ulRunTimeCounter, old = 0;
        for (int j = 0; j < nprev; j++) if (prev[j].h == ts[i].xHandle) { old = prev[j].rt; break; }
        now[i].h = ts[i].xHandle; now[i].rt = rt;
        ts[i].ulRunTimeCounter = prev_total ? rt - old : 0;
    }
    memcpy(prev, now, sizeof(prev_t) * n);
    nprev = n;
    prev_total = total;
    if (!window || window == total) return;                      // first call: baseline only
    qsort(ts, n, sizeof(TaskStatus_t), cmp_rt);

    float idle[2] = {0, 0};
    char line[200];
    int len = 0;
    for (int i = 0; i < n; i++) {
        float pct = 100.0f * ts[i].ulRunTimeCounter / window;
        if (!strncmp(ts[i].pcTaskName, "IDLE", 4)) { idle[ts[i].pcTaskName[4] == '1'] = pct; continue; }
        int core = ts[i].xCoreID > 1 ? -1 : ts[i].xCoreID;
        len += snprintf(line + len, sizeof(line) - len, " %s(c%d p%u) %.1f%% %uB |",
                        ts[i].pcTaskName, core, (unsigned)ts[i].uxCurrentPriority, pct,
                        (unsigned)ts[i].usStackHighWaterMark);
        if (len > 130 || i == n - 1) { ESP_LOGI(TAG, "tasks:%s", line); len = 0; line[0] = 0; }
    }
    if (len) ESP_LOGI(TAG, "tasks:%s", line);
    ESP_LOGI(TAG, "cpu: core0 %.0f%% busy, core1 %.0f%% busy (window %lu s)",
             100 - idle[0], 100 - idle[1], (unsigned long)(window / 1000000));
}

static void bench_task(void *arg)
{
    bench_done = diag_bench();
    vTaskDelete(NULL);
}

// Bench now, in its own task: rendering needs the LVGL task's stack size, not the caller's (the test console's
// 4 KB overflowed and reset the board). Result in the log as usual ("diag: bench ..." or "bench postponed").
void diag_bench_request(void)
{
    xTaskCreatePinnedToCore(bench_task, "bench", 10240, NULL, 4, NULL, 1);
}

static void diag_task(void *arg)
{
    int period = (int)(intptr_t)arg;
    size_t int_lg_min = SIZE_MAX, dma_lg_min = SIZE_MAX;
    startup_info();
    report_tasks();                                               // baseline
    for (int tick = 1;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        // Largest free blocks: sampled every second, worst value reported (fragmentation)
        size_t a = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t b = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (a < int_lg_min) int_lg_min = a;
        if (b < dma_lg_min) dma_lg_min = b;
        // once, on the LVGL core, after boot settles; only while the weather screen is idle (see diag_bench)
        if (BENCH_AT_S && !bench_done && tick >= BENCH_AT_S && (tick - BENCH_AT_S) % 20 == 0 && tick < 3600)
            xTaskCreatePinnedToCore(bench_task, "bench", 10240, NULL, 4, NULL, 1);   // renders like the LVGL task (8 KB); 6 KB overflowed
        if (tick % period) continue;

        ESP_LOGI(TAG, "heap: internal %u KB free (min ever %u, largest block now %u / worst %u) | "
                 "DMA %u KB (largest now %u / worst %u) | PSRAM %u KB free (min ever %u, largest %u)",
                 KB(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 KB(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), KB(a), KB(int_lg_min),
                 KB(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)), KB(b), KB(dma_lg_min),
                 KB(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), KB(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
                 KB(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        int_lg_min = dma_lg_min = SIZE_MAX;

        display_stats_t d;
        display_get_stats(&d, true);
        ESP_LOGI(TAG, "display: %lu frames, render avg %.1f ms max %.1f ms, %.1f Mpx sent | "
                 "animation %.1f fps (%lu frames, worst gap %.0f ms) | LVGL lock wait max %.1f ms, "
                 "longest hold %.1f ms by %s",
                 (unsigned long)d.frames, d.frames ? d.render_us / 1000.0 / d.frames : 0, d.render_max_us / 1000.0,
                 d.pixels / 1e6, d.anim_us ? d.anim_frames * 1e6 / d.anim_us : 0, (unsigned long)d.anim_frames,
                 d.anim_gap_max_us / 1000.0, d.lvgl_wait_max_us / 1000.0, d.hold_max_us / 1000.0,
                 d.hold_task[0] ? d.hold_task : "-");
        report_tasks();
    }
}

void diag_mark(const char *stage)
{
    ESP_LOGI(TAG, "mark %-14s internal %u KB free (largest %u), DMA %u KB, PSRAM %u KB", stage,
             KB(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             KB(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             KB(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
             KB(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

// Full-screen render timing of each screen. Invisible: other screens are rendered without being sent
// to the panel; the panel transfer is timed by repainting the (unchanged) weather screen.
// Blocks the UI for about 1.5 s.
bool diag_bench(void)
{
    lv_obj_t *scr[6];
    const char *name[6];
    const int N = 5;
    display_lock(-1);
    lv_obj_t *was = lv_screen_active();
    lv_indev_t *in = lv_indev_get_next(NULL);
    // Only while the weather screen is idle: the last step repaints it on the panel, which would flash
    // over any other screen (seen once over the hourly view). Otherwise try again 20 s later.
    if (was != ui_main_screen() || (in && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED) || lv_anim_count_running()) {
        display_unlock();
        ESP_LOGI(TAG, "bench postponed (screen in use)");
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
    ESP_LOGI(TAG, "bench render-only full screen:%s", line);
    ESP_LOGI(TAG, "bench weather incl. panel transfer: %.1f ms/frame", with_panel);
    return true;
}

void diag_start(int period_s)
{
    xTaskCreatePinnedToCore(diag_task, "diag", 4096, (void *)(intptr_t)period_s, 1, NULL, 0);
}
