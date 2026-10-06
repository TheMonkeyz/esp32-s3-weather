#pragma once
#include "esp_codec_dev.h"
typedef struct { int port; int addr; void *bus_handle; } audio_codec_i2c_cfg_t;
typedef struct { float pa_voltage, codec_dac_voltage; } emu_hw_gain_t;
typedef struct {
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_gpio_if_t *gpio_if;
    esp_codec_dev_work_mode_t codec_mode;
    int pa_pin;
    bool use_mclk;
    emu_hw_gain_t hw_gain;
} es8311_codec_cfg_t;
const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(const audio_codec_i2c_cfg_t *cfg);
const audio_codec_gpio_if_t *audio_codec_new_gpio(void);
const audio_codec_if_t *es8311_codec_new(const es8311_codec_cfg_t *cfg);
// The microphones (presence.c)
typedef struct { int port; void *rx_handle; void *tx_handle; } audio_codec_i2s_cfg_t;
const audio_codec_data_if_t *audio_codec_new_i2s_data(const audio_codec_i2s_cfg_t *cfg);
#define ES7210_SEL_MIC1 1
#define ES7210_SEL_MIC2 2
typedef struct { const audio_codec_ctrl_if_t *ctrl_if; int mic_selected; } es7210_codec_cfg_t;
const audio_codec_if_t *es7210_codec_new(const es7210_codec_cfg_t *cfg);
