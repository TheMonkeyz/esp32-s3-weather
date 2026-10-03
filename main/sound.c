// Alert chimes through the speaker (see sound.h).
// The ES8311 shares the I2S bus with the microphones: presence.c opens it in both directions and hands over the
// data interface (presence_audio_data_if()). The speaker device is opened only while a chime plays, so the
// amplifier (GPIO 46) is off the rest of the time.
#include "sound.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "touch.h"
#include "presence.h"
#include "config.h"

static const char *TAG = "sound";
#define RATE        16000                 // same as the microphones (one I2S clock for both)
#define PIN_PA      46
#define ES8311_ADDR 0x30                  // 8-bit, as esp_codec_dev expects

static sound_cfg_t cfg = { .level = 2, .volume = 60, .quiet_from = 22 * 60, .quiet_to = 7 * 60 };
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static esp_codec_dev_handle_t spk;
static QueueHandle_t q;                   // sound requests: level 1 yellow, 2 orange, 3 red

/* ---------- settings ---------- */

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open("sound", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    uint16_t m;
    if (nvs_get_u8(h, "level", &v) == ESP_OK && v <= 3) cfg.level = v;
    if (nvs_get_u8(h, "vol", &v) == ESP_OK && v <= 100) cfg.volume = v;
    if (nvs_get_u16(h, "qfrom", &m) == ESP_OK && m < 1440) cfg.quiet_from = m;
    if (nvs_get_u16(h, "qto", &m) == ESP_OK && m < 1440) cfg.quiet_to = m;
    nvs_close(h);
}

void sound_get_config(sound_cfg_t *out)
{
    taskENTER_CRITICAL(&mux);
    *out = cfg;
    taskEXIT_CRITICAL(&mux);
}

bool sound_set_config(const sound_cfg_t *in)
{
    sound_cfg_t c = *in;
    if (c.level < 0 || c.level > 3) c.level = 2;
    c.volume = c.volume < 0 ? 0 : c.volume > 100 ? 100 : c.volume;
    if (c.quiet_from < 0 || c.quiet_from >= 1440) c.quiet_from = 22 * 60;
    if (c.quiet_to < 0 || c.quiet_to >= 1440) c.quiet_to = 7 * 60;
    taskENTER_CRITICAL(&mux);
    cfg = c;
    taskEXIT_CRITICAL(&mux);
    nvs_handle_t h;
    bool ok = nvs_check(nvs_open("sound", NVS_READWRITE, &h), "open sound");
    if (ok) {
        ok = nvs_check(nvs_set_u8(h, "level", c.level), "sound/level") && nvs_check(nvs_set_u8(h, "vol", c.volume), "sound/vol") &&
             nvs_check(nvs_set_u16(h, "qfrom", c.quiet_from), "sound/qfrom") &&
             nvs_check(nvs_set_u16(h, "qto", c.quiet_to), "sound/qto") && nvs_check(nvs_commit(h), "sound commit");
        nvs_close(h);
    }
    ESP_LOGI(TAG, "config: level %d, volume %d%%, quiet %02d:%02d-%02d:%02d%s", c.level, c.volume,
             c.quiet_from / 60, c.quiet_from % 60, c.quiet_to / 60, c.quiet_to % 60, ok ? "" : " (NOT saved)");
    return ok;
}

// A speaker: the I2S bus is up (the microphones opened it) and the ES8311 answers on I2C (probed once at start).
// It meant only the first, so a board without the codec said it could beep.
static bool spk_found;
bool sound_ok(void) { return spk_found && presence_audio_data_if() != NULL; }

/* ---------- alert sounds ----------
 * Beeps whose urgency follows the alert level (not a doorbell, and not Canada's official Alert Ready signal):
 *   1 yellow / statement: two short beeps; 2 orange: two groups of three quick beeps; 3 red: hi-lo siren, ~3 s.
 * Tones at 1.35-1.8 kHz (where the small speaker is loudest), a sine plus some 3rd harmonic so it cuts through,
 * 6 ms soft edges (no clicks). The whole sound is rendered into PSRAM first and then streamed, so CPU load
 * elsewhere (Wi-Fi, radar) can't starve the I2S DMA and crackle. */

typedef struct { float at, len, f; } beep_t;

static int pattern(int level, beep_t *b, int max)
{
    int n = 0;
    if (level >= 3) {                                    // red: hi-lo siren
        for (int k = 0; k < 14 && n < max; k++) b[n++] = (beep_t){ 0.22f * k, 0.20f, k % 2 ? 1350 : 1800 };
    } else if (level == 2) {                             // orange: 3 + 3 quick beeps
        for (int g = 0; g < 2; g++)
            for (int k = 0; k < 3 && n < max; k++) b[n++] = (beep_t){ 0.75f * g + 0.18f * k, 0.12f, 1800 };
    } else {                                             // yellow / statement: 2 beeps
        for (int k = 0; k < 2 && n < max; k++) b[n++] = (beep_t){ 0.28f * k, 0.16f, 1500 };
    }
    return n;
}

#define LEAD_S 0.06f                                     // silence before / after (amplifier settles, no pop)
#define EDGE_S 0.006f
#define AMP    0.32f                                     // well below clipping, the codec volume does the rest

static int16_t *render(int level, int *frames)
{
    beep_t b[16];
    int n = pattern(level, b, 16);
    float end = 0;
    for (int i = 0; i < n; i++) if (b[i].at + b[i].len > end) end = b[i].at + b[i].len;
    int total = (int)((end + 2 * LEAD_S) * RATE);
    int16_t *pcm = heap_caps_calloc(total * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) return NULL;
    for (int i = 0; i < n; i++) {
        int s0 = (int)((b[i].at + LEAD_S) * RATE), len = (int)(b[i].len * RATE), edge = (int)(EDGE_S * RATE);
        for (int k = 0; k < len && s0 + k < total; k++) {
            float t = (float)k / RATE, w = 2 * (float)M_PI * b[i].f * t;
            float env = k < edge ? 0.5f - 0.5f * cosf((float)M_PI * k / edge)
                      : k > len - edge ? 0.5f - 0.5f * cosf((float)M_PI * (len - k) / edge) : 1.0f;
            int16_t v = (int16_t)(AMP * env * (sinf(w) + 0.3f * sinf(3 * w)) / 1.3f * 32767);
            pcm[2 * (s0 + k)] = pcm[2 * (s0 + k) + 1] = v;
        }
    }
    *frames = total;
    return pcm;
}

static bool spk_open(void)
{
    if (spk) return true;
    const audio_codec_data_if_t *data_if = presence_audio_data_if();
    if (!data_if) return false;
    audio_codec_i2c_cfg_t ccfg = { .port = 0, .addr = ES8311_ADDR, .bus_handle = touch_i2c_bus() };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ccfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!ctrl_if || !gpio_if) return false;
    es8311_codec_cfg_t ecfg = {
        .ctrl_if = ctrl_if, .gpio_if = gpio_if, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PIN_PA, .use_mclk = true, .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&ecfg);
    if (!codec) return false;
    esp_codec_dev_cfg_t dcfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    spk = esp_codec_dev_new(&dcfg);
    return spk != NULL;
}

static void play(int level)
{
    if (!spk_open()) { ESP_LOGW(TAG, "no speaker"); return; }
    int frames;
    int16_t *pcm = render(level, &frames);
    if (!pcm) { ESP_LOGW(TAG, "no memory for the sound"); return; }
    sound_cfg_t c;
    sound_get_config(&c);
    esp_codec_dev_sample_info_t fs = { .sample_rate = RATE, .channel = 2, .bits_per_sample = 16 };
    if (esp_codec_dev_open(spk, &fs) != ESP_CODEC_DEV_OK) { ESP_LOGW(TAG, "speaker open failed"); free(pcm); return; }
    esp_codec_dev_set_out_vol(spk, c.volume);
    enum { CHUNK = 1024 };                               // frames per write (4 KB)
    for (int n = 0; n < frames; n += CHUNK) {
        int k = frames - n < CHUNK ? frames - n : CHUNK;
        esp_codec_dev_write(spk, pcm + 2 * n, k * 2 * sizeof(int16_t));
    }
    esp_codec_dev_close(spk);                            // amplifier off
    free(pcm);
}

static void sound_task(void *arg)
{
    int level;
    while (1) {
        if (xQueueReceive(q, &level, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "alert sound, level %d", level);
            play(level);
        }
    }
}

static void request(int level)
{
    if (q) xQueueSend(q, &level, 0);
}

void sound_test(int level) { request(level < 1 ? 1 : level > 3 ? 3 : level); }

void sound_alert(char colour)
{
    sound_cfg_t c;
    sound_get_config(&c);
    int sev = colour == 'r' ? 3 : colour == 'o' ? 2 : colour == 'y' ? 1 : 0;
    int min = c.level == 1 ? 3 : c.level == 2 ? 2 : 0;
    if (c.level == 0 || sev < min) { ESP_LOGI(TAG, "new alert (%c): no chime at this level", colour); return; }
    struct tm tm;
    if (sev < 3 && c.quiet_from != c.quiet_to && config_local_time((long)time(NULL), &tm)) {
        int now = tm.tm_hour * 60 + tm.tm_min;
        bool quiet = c.quiet_from < c.quiet_to ? now >= c.quiet_from && now < c.quiet_to
                                               : now >= c.quiet_from || now < c.quiet_to;   // across midnight
        if (quiet) { ESP_LOGI(TAG, "new alert (%c): quiet hours", colour); return; }
    }
    request(sev < 1 ? 1 : sev);
}

void sound_start(void)
{
    load();
    i2c_master_bus_handle_t bus = touch_i2c_bus();
    spk_found = bus && i2c_master_probe(bus, ES8311_ADDR >> 1, 50) == ESP_OK;
    if (!spk_found) ESP_LOGW(TAG, "no ES8311 on I2C: no speaker");
    q = xQueueCreate(2, sizeof(int));
    xTaskCreatePinnedToCore(sound_task, "sound", 4096, NULL, 3, NULL, 0);
}
