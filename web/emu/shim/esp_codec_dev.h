#pragma once
// Browser emulator: the speaker (ES8311 through esp_codec_dev) is Web Audio, the microphones (ES7210) the browser's
// microphone when the page has it (web/emu/emu_audio.c)
#include <stdbool.h>
#include <stdint.h>
typedef struct emu_codec *esp_codec_dev_handle_t;
typedef struct { int dummy; } audio_codec_data_if_t;
typedef struct { int dummy; } audio_codec_ctrl_if_t;
typedef struct { int dummy; } audio_codec_gpio_if_t;
typedef struct { int dummy; } audio_codec_if_t;
typedef enum { ESP_CODEC_DEV_TYPE_IN = 1, ESP_CODEC_DEV_TYPE_OUT = 2 } esp_codec_dev_type_t;
typedef enum { ESP_CODEC_DEV_WORK_MODE_ADC = 1, ESP_CODEC_DEV_WORK_MODE_DAC = 2 } esp_codec_dev_work_mode_t;
#define ESP_CODEC_DEV_OK 0
typedef struct {
    esp_codec_dev_type_t dev_type;
    const audio_codec_if_t *codec_if;
    const audio_codec_data_if_t *data_if;
} esp_codec_dev_cfg_t;
typedef struct { uint32_t sample_rate; uint8_t channel; uint8_t bits_per_sample; } esp_codec_dev_sample_info_t;
esp_codec_dev_handle_t esp_codec_dev_new(const esp_codec_dev_cfg_t *cfg);
int esp_codec_dev_open(esp_codec_dev_handle_t h, esp_codec_dev_sample_info_t *fs);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t h, int volume);
int esp_codec_dev_write(esp_codec_dev_handle_t h, void *data, int len);
int esp_codec_dev_close(esp_codec_dev_handle_t h);
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t h, float db);
int esp_codec_dev_read(esp_codec_dev_handle_t h, void *data, int len);
