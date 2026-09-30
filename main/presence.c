// Makeshift presence sensor: the board's microphones (ES7210 via I2S) drive the screen brightness.
//   ACTIVE --(quiet for dim_s)--> DIM --(quiet for off_s more)--> OFF
//   DIM/OFF --(noise sustained for wake_s)--> ACTIVE      (a single bang doesn't wake it)
//   A touch always wakes it (and the touch that wakes an OFF screen is swallowed).
#include "presence.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "display.h"
#include "touch.h"

static const char *TAG = "presence";

#define SAMPLE_RATE   16000
#define TICK_MS       100                        // analysis window
#define FRAMES        (SAMPLE_RATE * TICK_MS / 1000)
#define PIN_MCLK      42
#define PIN_BCLK      9
#define PIN_WS        45
#define PIN_DIN       10                         // ES7210 -> ESP32 (the BSP calls it DSIN)
#define ES7210_ADDR   0x80                       // 8-bit address as esp_codec_dev expects
#define CALIB_MAX     600                        // up to 60 s of 100 ms samples

static presence_cfg_t cfg = {
    .enabled = true, .margin_db = 10, .wake_s = 2, .dim_s = 10, .off_s = 20,
    .bright_pct = 100, .dim_pct = 15, .baseline_db = -60,
};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static esp_codec_dev_handle_t mic;
static bool mic_ok;

static volatile presence_state_t state = PRESENCE_ACTIVE;
static volatile float level_db = -90, score, quiet_s;
static volatile int cur_pct = -1;                 // brightness actually applied
static volatile bool calibrating;
static volatile int calib_left_ticks;
static float *calib_buf;
static int calib_n;

/* ---------------- settings ---------------- */

static void load_cfg(void)
{
    nvs_handle_t h;
    if (nvs_open("presence", NVS_READONLY, &h) != ESP_OK) return;
    presence_cfg_t c = cfg;
    size_t len = sizeof(c);
    if (nvs_get_blob(h, "cfg", &c, &len) == ESP_OK && len == sizeof(c)) cfg = c;
    nvs_close(h);
}

static void save_cfg(void)
{
    nvs_handle_t h;
    if (nvs_open("presence", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", &cfg, sizeof(cfg));
    nvs_commit(h);
    nvs_close(h);
}

void presence_get_config(presence_cfg_t *out)
{
    taskENTER_CRITICAL(&mux);
    *out = cfg;
    taskEXIT_CRITICAL(&mux);
}

void presence_set_config(const presence_cfg_t *in)
{
    presence_cfg_t c = *in;
    c.baseline_db = cfg.baseline_db;              // only calibration changes the baseline
    if (c.margin_db < 1) c.margin_db = 1;
    if (c.margin_db > 60) c.margin_db = 60;
    if (c.wake_s < 0.2f) c.wake_s = 0.2f;
    if (c.dim_s < 1) c.dim_s = 1;
    if (c.off_s < 1) c.off_s = 1;
    if (c.bright_pct < 5) c.bright_pct = 5;
    if (c.bright_pct > 100) c.bright_pct = 100;
    if (c.dim_pct < 1) c.dim_pct = 1;
    if (c.dim_pct > c.bright_pct) c.dim_pct = c.bright_pct;
    taskENTER_CRITICAL(&mux);
    cfg = c;
    taskEXIT_CRITICAL(&mux);
    save_cfg();
    presence_wake();                              // show the result of the new settings right away
    ESP_LOGI(TAG, "config: %s, margin %.0f dB, wake %.1f s, dim %.0f s, off +%.0f s, %d%%/%d%%",
             c.enabled ? "on" : "off", c.margin_db, c.wake_s, c.dim_s, c.off_s, c.bright_pct, c.dim_pct);
}

/* ---------------- status / control ---------------- */

void presence_get_status(presence_status_t *st)
{
    st->level_db = level_db;
    st->threshold_db = cfg.baseline_db + cfg.margin_db;
    st->state = state;
    st->wake_progress = cfg.wake_s > 0 ? score / cfg.wake_s : 0;
    st->quiet_s = quiet_s;
    st->calibrating = calibrating;
    st->calib_left_s = calib_left_ticks * TICK_MS / 1000.0f;
    st->mic_ok = mic_ok;
    st->brightness = cur_pct < 0 ? 0 : cur_pct;
}

bool presence_calibrate(int seconds)
{
    if (!mic_ok || calibrating) return false;
    if (seconds < 2) seconds = 2;
    if (seconds > CALIB_MAX * TICK_MS / 1000) seconds = CALIB_MAX * TICK_MS / 1000;
    calib_n = 0;
    calib_left_ticks = seconds * 1000 / TICK_MS;
    calibrating = true;
    ESP_LOGI(TAG, "calibrating for %d s - keep quiet", seconds);
    return true;
}

void presence_wake(void)
{
    state = PRESENCE_ACTIVE;
    quiet_s = 0;
}

bool presence_touch(void)
{
    bool was_off = state == PRESENCE_OFF;
    presence_wake();
    return was_off;
}

bool presence_screen_off(void) { return state == PRESENCE_OFF; }

/* ---------------- audio ---------------- */

static bool mic_init(void)
{
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = 320;
    if (i2s_new_channel(&cc, NULL, &rx) != ESP_OK) return false;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = I2S_GPIO_UNUSED, .din = PIN_DIN },
    };
    if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) return false;

    audio_codec_i2s_cfg_t icfg = { .port = I2S_NUM_0, .rx_handle = rx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&icfg);
    audio_codec_i2c_cfg_t ccfg = { .port = 0, .addr = ES7210_ADDR, .bus_handle = touch_i2c_bus() };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ccfg);
    if (!data_if || !ctrl_if) return false;
    es7210_codec_cfg_t ecfg = { .ctrl_if = ctrl_if, .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 };
    const audio_codec_if_t *codec = es7210_codec_new(&ecfg);
    if (!codec) return false;
    esp_codec_dev_cfg_t dcfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    mic = esp_codec_dev_new(&dcfg);
    if (!mic) return false;
    esp_codec_dev_set_in_gain(mic, 30.0);
    esp_codec_dev_sample_info_t fs = { .sample_rate = SAMPLE_RATE, .channel = 2, .bits_per_sample = 16 };
    return esp_codec_dev_open(mic, &fs) == ESP_CODEC_DEV_OK;
}

static void apply_brightness(int target)
{
    if (target == cur_pct) return;
    // Fade: ~1 s from full to off
    int step = 10;
    int next = cur_pct < 0 ? target : (target > cur_pct ? (cur_pct + step > target ? target : cur_pct + step)
                                                          : (cur_pct - step < target ? target : cur_pct - step));
    display_lock(-1);
    display_brightness((uint8_t)(next * 255 / 100));
    display_unlock();
    cur_pct = next;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void presence_task(void *arg)
{
    int16_t *buf = heap_caps_malloc(FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    calib_buf = heap_caps_malloc(CALIB_MAX * sizeof(float), MALLOC_CAP_SPIRAM);
    mic_ok = buf && calib_buf && mic_init();
    ESP_LOGI(TAG, "microphones %s, baseline %.1f dBFS", mic_ok ? "ready" : "NOT available", cfg.baseline_db);
    const float dt = TICK_MS / 1000.0f;
    uint32_t log_tick = 0;
    presence_state_t last_state = state;

    while (1) {
        if (mic_ok) {
            if (esp_codec_dev_read(mic, buf, FRAMES * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
                vTaskDelay(pdMS_TO_TICKS(TICK_MS));
                continue;
            }
            double acc = 0;
            for (int i = 0; i < FRAMES * 2; i++) acc += (double)buf[i] * buf[i];
            double rms = sqrt(acc / (FRAMES * 2));
            float db = rms > 0.5 ? 20.0f * log10f((float)(rms / 32768.0)) : -90.0f;
            level_db = db;
        } else {
            vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        }

        // Calibration: baseline = 90th percentile of the quiet room's levels
        if (calibrating) {
            if (calib_n < CALIB_MAX) calib_buf[calib_n++] = level_db;
            if (--calib_left_ticks <= 0) {
                qsort(calib_buf, calib_n, sizeof(float), cmp_float);
                float p90 = calib_buf[(int)(calib_n * 0.9f)];
                taskENTER_CRITICAL(&mux);
                cfg.baseline_db = p90;
                taskEXIT_CRITICAL(&mux);
                save_cfg();
                calibrating = false;
                ESP_LOGI(TAG, "calibrated: baseline %.1f dBFS (min %.1f, max %.1f, %d samples)",
                         p90, calib_buf[0], calib_buf[calib_n - 1], calib_n);
            }
        }

        presence_cfg_t c;
        presence_get_config(&c);
        bool loud = mic_ok && !calibrating && level_db > c.baseline_db + c.margin_db;

        // Sustained-noise score: rises while loud, falls at half speed while quiet
        float s = score + (loud ? dt : -dt * 0.5f);
        score = s < 0 ? 0 : (s > c.wake_s ? c.wake_s : s);

        if (!c.enabled || !mic_ok) {
            state = PRESENCE_ACTIVE;
            quiet_s = 0;
        } else {
            switch (state) {
            case PRESENCE_ACTIVE:
                quiet_s = loud ? 0 : quiet_s + dt;
                if (quiet_s >= c.dim_s) state = PRESENCE_DIM;
                break;
            case PRESENCE_DIM:
                if (score >= c.wake_s) { state = PRESENCE_ACTIVE; quiet_s = 0; break; }
                quiet_s = loud ? c.dim_s : quiet_s + dt;         // a short noise restarts the off countdown
                if (quiet_s >= c.dim_s + c.off_s) state = PRESENCE_OFF;
                break;
            case PRESENCE_OFF:
                if (score >= c.wake_s) { state = PRESENCE_ACTIVE; quiet_s = 0; }
                break;
            }
        }
        if (state != last_state) {
            static const char *names[] = {"ACTIVE", "DIM", "OFF"};
            ESP_LOGI(TAG, "%s -> %s (level %.1f dB, threshold %.1f dB)", names[last_state], names[state],
                     level_db, c.baseline_db + c.margin_db);
            last_state = state;
        }
        apply_brightness(state == PRESENCE_ACTIVE ? c.bright_pct : state == PRESENCE_DIM ? c.dim_pct : 0);

        if (++log_tick % 50 == 0)                              // every 5 s
            ESP_LOGI(TAG, "level %.1f dB (threshold %.1f), score %.1f/%.1f, quiet %.0f s",
                     level_db, c.baseline_db + c.margin_db, score, c.wake_s, quiet_s);
    }
}

void presence_start(void)
{
    load_cfg();
    xTaskCreatePinnedToCore(presence_task, "presence", 4096, NULL, 2, NULL, 0);
}
