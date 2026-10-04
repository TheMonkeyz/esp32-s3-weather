// The speaker, in the browser: esp_codec_dev for sound.c. Its PCM (16 kHz, 16-bit stereo, written in pieces between
// open and close) is collected and played through Web Audio at the codec volume when the speaker is closed. Browsers
// allow sound only after a user gesture: index.html unlocks the AudioContext on the first touch of the screen.
#include <stdlib.h>
#include <emscripten.h>
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

struct emu_codec { int rate, channels, volume; };
static const audio_codec_ctrl_if_t ctrl;
static const audio_codec_gpio_if_t gpio;
static const audio_codec_if_t codec;

const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(const audio_codec_i2c_cfg_t *cfg) { (void)cfg; return &ctrl; }
const audio_codec_gpio_if_t *audio_codec_new_gpio(void) { return &gpio; }
const audio_codec_if_t *es8311_codec_new(const es8311_codec_cfg_t *cfg) { (void)cfg; return &codec; }

esp_codec_dev_handle_t esp_codec_dev_new(const esp_codec_dev_cfg_t *cfg)
{
    (void)cfg;
    struct emu_codec *c = calloc(1, sizeof(*c));
    if (c) c->volume = 60;
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
    js_audio_begin();
    return ESP_CODEC_DEV_OK;
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
