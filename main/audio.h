#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The board's audio: one I2S port (I2S0, master, 16 kHz, 16-bit stereo) in both directions, as the two codecs share
// its clocks (MCLK 42, BCLK 9, WS 45): DIN 10 from the ES7210 (two microphones, I2C 0x40), DOUT 8 to the ES8311
// (speaker, sound.c). Adapted from espforge's boards/ws_amoled175/board/audio.c (this display has its own board code);
// until v1.15.0 it was in presence.c. The codecs' I2C is touch.c's bus: after touch_init().

// The I2S port, both directions (the speaker is silent until something plays). Called by app_main before the
// presence and sound tasks start; audio_mic_open() calls it too. false = I2S couldn't be set up.
bool audio_init(void);
// The microphones (ES7210, both channels, gain in dB: 30 for a room's background noise), then reads of n int16
// samples (interleaved left/right), blocking until they are there. false = not available / a failed read.
// forge_presence's mic_open / mic_read hooks (main.c).
bool audio_mic_open(float gain_db);
bool audio_mic_read(int16_t *samples, size_t n);
// The shared I2S data interface (a const audio_codec_data_if_t *) for the speaker's own ES8311 device (sound.c);
// NULL before audio_init() or if it failed
const void *audio_data_if(void);
