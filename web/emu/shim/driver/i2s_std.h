#pragma once
// Browser emulator: the I2S bus main/audio.c opens for the microphones and the speaker. Nothing to open here: the
// microphones are the browser's (web/emu/emu_audio.c, esp_codec_dev_read) and the speaker is Web Audio.
#include <stdbool.h>
#include "esp_err.h"
typedef struct emu_i2s_chan *i2s_chan_handle_t;
typedef enum { I2S_NUM_0 = 0 } i2s_port_t;
typedef enum { I2S_ROLE_MASTER = 0 } i2s_role_t;
typedef enum { I2S_DATA_BIT_WIDTH_16BIT = 16 } i2s_data_bit_width_t;
typedef enum { I2S_SLOT_MODE_MONO = 1, I2S_SLOT_MODE_STEREO = 2 } i2s_slot_mode_t;
typedef struct { i2s_port_t id; i2s_role_t role; int dma_desc_num, dma_frame_num; bool auto_clear; } i2s_chan_config_t;
typedef struct { unsigned sample_rate_hz; } i2s_std_clk_config_t;
typedef struct { i2s_data_bit_width_t width; i2s_slot_mode_t mode; } i2s_std_slot_config_t;
typedef struct { int mclk, bclk, ws, dout, din; } i2s_std_gpio_config_t;
typedef struct { i2s_std_clk_config_t clk_cfg; i2s_std_slot_config_t slot_cfg; i2s_std_gpio_config_t gpio_cfg; } i2s_std_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(p_, r_) { .id = (p_), .role = (r_), .dma_desc_num = 6, .dma_frame_num = 240 }
#define I2S_STD_CLK_DEFAULT_CONFIG(rate_) { .sample_rate_hz = (rate_) }
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(w_, m_) { .width = (w_), .mode = (m_) }
static inline esp_err_t i2s_new_channel(const i2s_chan_config_t *c, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    (void)c;
    static int t, r;
    if (tx) *tx = (i2s_chan_handle_t)&t;
    if (rx) *rx = (i2s_chan_handle_t)&r;
    return ESP_OK;
}
static inline esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t h, const i2s_std_config_t *c) { (void)h; (void)c; return ESP_OK; }
static inline esp_err_t i2s_channel_enable(i2s_chan_handle_t h) { (void)h; return ESP_OK; }
