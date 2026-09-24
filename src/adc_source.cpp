#include "config.h"

#if AUDIO_SOURCE == AUDIO_SRC_ADC

#include "audio_source.h"

#include "esp_adc/adc_continuous.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "ADC_SRC";

// One DMA frame = 256 conversions of SOC_ADC_DIGI_RESULT_BYTES each.
#define ADC_FRAME_SAMPLES   256
#define ADC_FRAME_BYTES     (ADC_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)

static adc_continuous_handle_t adc_handle = nullptr;
static adc_channel_t adc_channel;

// Bias tracker: one-pole low-pass, tau = 4096 samples (~85 ms at 48 kHz, ~170 ms at 24 kHz).
static float bias = -1.0f;

void audio_source_init()
{
    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_continuous_io_to_channel(ADC_INPUT_GPIO, &unit, &adc_channel));
    if (unit != ADC_UNIT_1) {
        // ADC2 is shared with Wi-Fi and not usable in continuous mode here.
        ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin", ADC_INPUT_GPIO);
        abort();
    }

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = ADC_FRAME_BYTES * 8,
        .conv_frame_size = ADC_FRAME_BYTES,
        .flags = {
            .flush_pool = 1, // if analysis falls behind, drop old data instead of stalling
        },
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_handle));

    adc_digi_pattern_config_t pattern = {
        .atten = ADC_INPUT_ATTEN,
        .channel = (uint8_t)adc_channel,
        .unit = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };

    adc_continuous_config_t cfg = {};
    cfg.pattern_num = 1;
    cfg.adc_pattern = &pattern;
    cfg.sample_freq_hz = AUDIO_SAMPLE_RATE;
    cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &cfg));

    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));

    ESP_LOGI(TAG, "ADC started: GPIO%d (ADC1_CH%d), Fs=%d Hz, 12-bit, atten %d",
             ADC_INPUT_GPIO, (int)adc_channel, AUDIO_SAMPLE_RATE, ADC_INPUT_ATTEN);
}

size_t audio_source_read(int32_t *out, size_t max)
{
    static uint8_t raw[ADC_FRAME_BYTES];
    static adc_continuous_data_t parsed[ADC_FRAME_SAMPLES];

    uint32_t bytes = 0;
    uint32_t want = (max < ADC_FRAME_SAMPLES ? max : ADC_FRAME_SAMPLES) * SOC_ADC_DIGI_RESULT_BYTES;

    esp_err_t err = adc_continuous_read(adc_handle, raw, want, &bytes, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC read: %s", esp_err_to_name(err));
        return 0;
    }

    uint32_t count = 0;
    ESP_ERROR_CHECK(adc_continuous_parse_data(adc_handle, raw, bytes, parsed, &count));

    size_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!parsed[i].valid || parsed[i].channel != adc_channel)
            continue;

        const float v = (float)parsed[i].raw_data;
        if (bias < 0.0f)
            bias = v; // start centred, no power-up step
        bias += (v - bias) * (1.0f / 4096.0f);

        out[n++] = (int32_t)((v - bias) * (AUDIO_FULL_SCALE / 2048.0f));
    }
    return n;
}

void audio_source_log()
{
}

#endif // AUDIO_SOURCE == AUDIO_SRC_ADC
