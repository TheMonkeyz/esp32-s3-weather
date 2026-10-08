// The speaker and the microphones, in the browser: esp_codec_dev for sound.c and audio.c (forge_presence's microphones).
// Speaker: its PCM (16 kHz, 16-bit stereo, written in pieces between open and close) is collected and played through
// Web Audio at the codec volume when the speaker is closed. Browsers allow sound only after a user gesture: index.html
// unlocks the AudioContext on the first touch of the screen.
// Microphones: the browser's microphone once the visitor turns it on (index.html, "Use my microphone"), resampled to
// 16 kHz there; forge_presence reads 100 ms at a time (audio_mic_read) as from the ES7210. Without it, silence at the same pace.
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

struct emu_codec { int rate, channels, volume; bool in; };
static const audio_codec_ctrl_if_t ctrl;
static const audio_codec_gpio_if_t gpio;
static const audio_codec_if_t codec;

const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(const audio_codec_i2c_cfg_t *cfg) { (void)cfg; return &ctrl; }
const audio_codec_gpio_if_t *audio_codec_new_gpio(void) { return &gpio; }
const audio_codec_if_t *es8311_codec_new(const es8311_codec_cfg_t *cfg) { (void)cfg; return &codec; }
static const audio_codec_data_if_t i2s_data;
const audio_codec_data_if_t *audio_codec_new_i2s_data(const audio_codec_i2s_cfg_t *cfg) { (void)cfg; return &i2s_data; }
const audio_codec_if_t *es7210_codec_new(const es7210_codec_cfg_t *cfg) { (void)cfg; return &codec; }

esp_codec_dev_handle_t esp_codec_dev_new(const esp_codec_dev_cfg_t *cfg)
{
    struct emu_codec *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->volume = 60;
    c->in = cfg && cfg->dev_type == ESP_CODEC_DEV_TYPE_IN;
    return c;
}

EM_JS(void, js_audio_begin, (void), { Module.emuPcm = []; });
EM_JS(void, js_audio_add, (const int16_t *p, int n), { Module.emuPcm.push(HEAP16.slice(p >> 1, (p >> 1) + n)); });
EM_JS(void, js_audio_play, (int rate, int channels, int volume), {
    const ctx = Module.emuAudio;                   // made by index.html on the first touch
    const parts = Module.emuPcm || [];
    Module.emuPcm = [];
    if (!ctx || !parts.length) { console.log('sound: no audio yet (touch the screen once)'); return; }
    const n = parts.reduce((a, p) => a + p.length, 0) / channels;
    const buf = ctx.createBuffer(channels, n, rate);
    for (let ch = 0; ch < channels; ch++) {
        const out = buf.getChannelData(ch);
        let i = 0;
        for (const p of parts) for (let k = ch; k < p.length; k += channels) out[i++] = p[k] / 32768;
    }
    const src = ctx.createBufferSource(), gain = ctx.createGain();
    gain.gain.value = volume / 100;
    src.buffer = buf;
    src.connect(gain).connect(ctx.destination);
    src.start();
    console.log('sound: played ' + (n / rate).toFixed(2) + ' s at volume ' + volume);
});

int esp_codec_dev_open(esp_codec_dev_handle_t h, esp_codec_dev_sample_info_t *fs)
{
    h->rate = (int)fs->sample_rate;
    h->channels = fs->channel;
    if (!h->in) js_audio_begin();
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t h, float db) { (void)h; (void)db; return ESP_CODEC_DEV_OK; }

// `frames` mono samples from the page's microphone queue (16 kHz) into `out` as 16-bit stereo: -1 without a microphone,
// 0 while fewer have arrived
EM_JS(int, js_mic_take, (int16_t *out, int frames), {
    const m = Module.emuMic;
    if (!m || !m.on) return -1;
    if (m.n < frames) return 0;
    let o = out >> 1, need = frames;
    while (need > 0) {
        const c = m.q[0], k = Math.min(need, c.length - m.off);
        for (let i = 0; i < k; i++) { const v = c[m.off + i]; HEAP16[o++] = v; HEAP16[o++] = v; }
        m.off += k; need -= k;
        if (m.off >= c.length) { m.q.shift(); m.off = 0; }
    }
    m.n -= frames;
    return frames;
});

// As the ES7210 through I2S: returns when `len` bytes (16-bit stereo) have been heard, ~100 ms for forge_presence
int esp_codec_dev_read(esp_codec_dev_handle_t h, void *data, int len)
{
    int frames = len / 4, ms = frames * 1000 / (h->rate > 0 ? h->rate : 16000);
    int64_t give_up = esp_timer_get_time() + (ms + 300) * 1000LL;
    for (;;) {
        int got = js_mic_take(data, frames);
        if (got > 0) return ESP_CODEC_DEV_OK;
        if (got < 0 || esp_timer_get_time() > give_up) {          // no microphone (or it stalled): silence, on time
            memset(data, 0, len);
            if (got < 0) vTaskDelay(pdMS_TO_TICKS(ms));
            return ESP_CODEC_DEV_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));                             // wait for the browser to deliver more
    }
}

int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t h, int volume) { h->volume = volume; return ESP_CODEC_DEV_OK; }

int esp_codec_dev_write(esp_codec_dev_handle_t h, void *data, int len)
{
    (void)h;
    js_audio_add(data, len / 2);
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_close(esp_codec_dev_handle_t h)
{
    js_audio_play(h->rate, h->channels, h->volume);
    return ESP_CODEC_DEV_OK;
}
