#include <string.h>
// CO5300 466x466 AMOLED over QSPI (Waveshare ESP32-S3-Touch-AMOLED-1.75) + LVGL v9 port
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

/* ---- diagnostics: lock contention and frame timing (read by diag.c) ---- */
static display_stats_t st;
static TaskHandle_t lvgl_th;
static int lock_depth;                 // only touched by the mutex owner
static int64_t lock_t0, render_t0, last_render;

static const char *TAG = "display";

#define LCD_HOST   SPI2_HOST
#define PIN_CS     12
#define PIN_CLK    38
#define PIN_D0     4
#define PIN_D1     5
#define PIN_D2     6
#define PIN_D3     7
#define PIN_RST    39
#define X_GAP      6
#define BUF_LINES  32

static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t lvgl_mux;

#define CMD(c)  (((uint32_t)0x02 << 24) | ((uint32_t)(c) << 8))
#define PIXELS  (((uint32_t)0x32 << 24) | ((uint32_t)0x2C << 8))

static void lcd_cmd(uint8_t c, const uint8_t *d, size_t n)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, CMD(c), d, n));
}

typedef struct { uint8_t cmd; uint8_t data[4]; uint8_t len; uint16_t delay_ms; } init_cmd_t;

// Vendor init sequence from Waveshare's BSP
static const init_cmd_t init_cmds[] = {
    {0xFE, {0x20}, 1, 0}, {0x19, {0x10}, 1, 0}, {0x1C, {0xA0}, 1, 0},
    {0xFE, {0x00}, 1, 0}, {0xC4, {0x80}, 1, 0}, {0x3A, {0x55}, 1, 0},
    {0x35, {0x00}, 1, 0}, {0x53, {0x20}, 1, 0}, {0x51, {0xFF}, 1, 0},
    {0x63, {0xFF}, 1, 0},
    {0x2A, {0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, {0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, {0}, 0, 600},
    {0x29, {0}, 0, 0},
};

static bool on_trans_done(esp_lcd_panel_io_handle_t h, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static bool bench_no_panel;          // diag bench: render only, don't send to the panel

void display_bench_no_panel(bool on) { bench_no_panel = on; }

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    if (bench_no_panel) { lv_display_flush_ready(disp); return; }
    int x1 = a->x1 + X_GAP, x2 = a->x2 + X_GAP;
    uint8_t col[4] = {x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF};
    uint8_t row[4] = {a->y1 >> 8, a->y1 & 0xFF, a->y2 >> 8, a->y2 & 0xFF};
    lcd_cmd(0x2A, col, 4);
    lcd_cmd(0x2B, row, 4);
    uint32_t n = lv_area_get_size(a);
    st.pixels += n;
    lv_draw_sw_rgb565_swap(px, n);
    esp_lcd_panel_io_tx_color(io, PIXELS, px, n * 2);
}

// CO5300 needs even start / odd end coordinates
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1; a->y1 &= ~1;
    a->x2 |= 1;  a->y2 |= 1;
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }


bool display_lock(int timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    bool ok = xSemaphoreTakeRecursive(lvgl_mux, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (ok && ++lock_depth == 1) {
        lock_t0 = esp_timer_get_time();
        if (xTaskGetCurrentTaskHandle() == lvgl_th && lock_t0 - t0 > st.lvgl_wait_max_us)
            st.lvgl_wait_max_us = lock_t0 - t0;          // LVGL blocked by another task
    }
    return ok;
}

void display_unlock(void)
{
    if (--lock_depth == 0 && xTaskGetCurrentTaskHandle() != lvgl_th) {
        int64_t held = esp_timer_get_time() - lock_t0;
        if (held > st.hold_max_us) {
            st.hold_max_us = held;
            strlcpy(st.hold_task, pcTaskGetName(NULL), sizeof(st.hold_task));
        }
    }
    xSemaphoreGiveRecursive(lvgl_mux);
}

static void render_evt(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
        if (last_render && now - last_render < 250000) {     // back-to-back frames = animation
            st.anim_frames++;
            st.anim_us += now - last_render;
            if (now - last_render > st.anim_gap_max_us) st.anim_gap_max_us = now - last_render;
        }
        last_render = render_t0 = now;
    } else {
        uint32_t us = now - render_t0;
        st.frames++;
        st.render_us += us;
        if (us > st.render_max_us) st.render_max_us = us;
    }
}

void display_get_stats(display_stats_t *out, bool reset)
{
    display_lock(-1);
    *out = st;
    if (reset) memset(&st, 0, sizeof(st));
    display_unlock();
}

static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t wait = 10;
        if (display_lock(-1)) { wait = lv_timer_handler(); display_unlock(); }
        if (wait < 5) wait = 5;
        if (wait > 50) wait = 50;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

void display_init(void)
{
    ESP_LOGI(TAG, "Init QSPI bus + CO5300");
    size_t buf_bytes = DISP_W * BUF_LINES * 2;

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_CLK, .data0_io_num = PIN_D0, .data1_io_num = PIN_D1,
        .data2_io_num = PIN_D2, .data3_io_num = PIN_D3,
        .max_transfer_sz = buf_bytes,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(DISP_W, DISP_H);

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS, .dc_gpio_num = -1, .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000, .trans_queue_depth = 10,
        .on_color_trans_done = on_trans_done, .user_ctx = disp,
        .lcd_cmd_bits = 32, .lcd_param_bits = 8,
        .flags = { .quad_mode = true },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    gpio_config_t rst = { .pin_bit_mask = 1ULL << PIN_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(150));

    for (size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); i++) {
        lcd_cmd(init_cmds[i].cmd, init_cmds[i].len ? init_cmds[i].data : NULL, init_cmds[i].len);
        if (init_cmds[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }

    void *b1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *b2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(b1 && b2);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, b1, b2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_display_add_event_cb(disp, render_evt, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, render_evt, LV_EVENT_RENDER_READY, NULL);

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, &lvgl_th, 1);
    ESP_LOGI(TAG, "Display ready (%dx%d)", DISP_W, DISP_H);
}

void display_brightness(uint8_t level)
{
    lcd_cmd(0x51, &level, 1);
}
