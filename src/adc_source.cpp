#include "config.h"

#if AUDIO_SOURCE == AUDIO_SRC_ADC

#include "audio_source.h"

#include <inttypes.h>

#include "esp_adc/adc_continuous.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "ADC_SRC";

// One DMA frame = 256 conversions of SOC_ADC_DIGI_RESULT_BYTES each.
#define ADC_FRAME_SAMPLES   256
#define ADC_FRAME_BYTES     (ADC_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)

static adc_continuous_handle_t adc_handle = nullptr;
static adc_channel_t adc_channel;

// Frames the driver could not store (pool full: the reader stalled for more
// than the pool's length). Each one is a hole in the sample stream: left out,
// everything after it moves earlier in time, which shifts FAX lines sideways
// (and biases the measured sample rate). So the holes are recorded where they
// fall in the stream of stored bytes and filled with silence on reading.
// Without flush_pool the driver drops exactly the new frame, and calls
// on_conv_done (with its size) then on_pool_ovf for it.
constexpr int GAP_MAX = 32;
struct Gap {
    uint32_t at;       // stored bytes before the hole
    uint32_t bytes;
};
static Gap gaps[GAP_MAX];
static volatile uint32_t gap_head = 0;       // written by the ISR
static uint32_t gap_tail = 0;                // read by audio_source_read()
static uint32_t isr_stored = 0, isr_last = 0;
static volatile uint32_t lost_frames = 0, gaps_dropped = 0;
static uint32_t consumed = 0;                // bytes read from the pool
static uint32_t fill_left = 0;               // silence still to output, in samples
static uint32_t logged_lost = 0;
static portMUX_TYPE gap_mux = portMUX_INITIALIZER_UNLOCKED;

static bool IRAM_ATTR on_conv_done(adc_continuous_handle_t, const adc_continuous_evt_data_t *edata, void *)
{
    isr_last = edata->size;
    isr_stored += edata->size;    // undone below when it did not fit
    return false;
}

static bool IRAM_ATTR on_pool_ovf(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *)
{
    isr_stored -= isr_last;
    lost_frames = lost_frames + 1;
    portENTER_CRITICAL_ISR(&gap_mux);
    const uint32_t h = gap_head;
    Gap &prev = gaps[(h - 1) % GAP_MAX];
    if (h != gap_tail && prev.at == isr_stored) {
        prev.bytes += isr_last;    // consecutive frames: one longer hole
    } else if (h - gap_tail < GAP_MAX) {
        gaps[h % GAP_MAX] = { isr_stored, isr_last };
        gap_head = h + 1;
    } else {
        gaps_dropped = gaps_dropped + 1;
    }
    portEXIT_CRITICAL_ISR(&gap_mux);
    return false;
}

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
        .max_store_buf_size = ADC_FRAME_BYTES * 16,    // ~85 ms at 48 kHz
        .conv_frame_size = ADC_FRAME_BYTES,
        // Not flush_pool: it drops an unknown amount of old data at once.
        .flags = {
            .flush_pool = 0,
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

    adc_continuous_evt_cbs_t cbs = {};
    cbs.on_conv_done = on_conv_done;
    cbs.on_pool_ovf = on_pool_ovf;
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(adc_handle, &cbs, nullptr));

    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));

    ESP_LOGI(TAG, "ADC started: GPIO%d (ADC1_CH%d), Fs=%d Hz, 12-bit, atten %d",
             ADC_INPUT_GPIO, (int)adc_channel, AUDIO_SAMPLE_RATE, ADC_INPUT_ATTEN);
}

size_t audio_source_read(int32_t *out, size_t max)
{
    static uint8_t raw[ADC_FRAME_BYTES];
    static adc_continuous_data_t parsed[ADC_FRAME_SAMPLES];

    // A hole at the read position: output its silence first. Otherwise read
    // no further than the next hole.
    uint32_t want = (max < ADC_FRAME_SAMPLES ? max : ADC_FRAME_SAMPLES) * SOC_ADC_DIGI_RESULT_BYTES;
    if (!fill_left) {
        portENTER_CRITICAL(&gap_mux);
        const bool pending = gap_tail != gap_head;
        const Gap g = gaps[gap_tail % GAP_MAX];
        if (pending && g.at == consumed) {
            fill_left = g.bytes / SOC_ADC_DIGI_RESULT_BYTES;
            gap_tail++;
        }
        portEXIT_CRITICAL(&gap_mux);
        if (pending && g.at != consumed && g.at - consumed < want)
            want = g.at - consumed;
    }
    if (fill_left) {
        const size_t n = fill_left < max ? fill_left : max;
        for (size_t i = 0; i < n; i++)
            out[i] = 0;
        fill_left -= n;
        return n;
    }

    uint32_t bytes = 0;

    esp_err_t err = adc_continuous_read(adc_handle, raw, want, &bytes, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC read: %s", esp_err_to_name(err));
        return 0;
    }

    consumed += bytes;

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
    const uint32_t lost = lost_frames;
    if (lost == logged_lost)
        return;
    ESP_LOGW(TAG, "ADC: %" PRIu32 " frames perdidos (%" PRIu32 " no total), preenchidos com silencio%s",
             lost - logged_lost, lost, gaps_dropped ? " - fila de buracos cheia" : "");
    logged_lost = lost;
}

#endif // AUDIO_SOURCE == AUDIO_SRC_ADC
