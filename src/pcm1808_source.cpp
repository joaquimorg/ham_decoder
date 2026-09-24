#include "config.h"

#if AUDIO_SOURCE == AUDIO_SRC_PCM1808

#include "audio_source.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

static const char *TAG = "PCM1808";

// Stereo frames per DMA buffer.
#define PCM_DMA_FRAMES      256
#define PCM_DMA_DESC        16      // ~85 ms of buffering at 48 kHz

// PCM1808 startup (~190 ms) plus its internal DC high-pass settling.
#define PCM_STARTUP_DISCARD_MS  500

// Observed corruption: the top bits of the 24-bit word (the first ones after
// the LRCK edge) arrive wrong while the low 16 bits are intact, e.g. 0CA244
// read as FCA244. A sample that is a spike of more than GLITCH_THRESHOLD
// above (or below) BOTH neighbours is corrupt: a full-scale 3.5 kHz sine at
// 48 kHz moves at most ~0.4 M between samples, and steep real edges are
// monotonic, not spikes. The repair keeps the low 16 bits and picks
// the top byte that lands closest to the neighbours' mean.
#define GLITCH_THRESHOLD    (1 << 20)
#define GLITCH_LOW_BITS     16
#define GLITCH_EXACT_TOL    (1 << (GLITCH_LOW_BITS - 1))
#define GLITCH_REPORT_SAMPLES (AUDIO_SAMPLE_RATE * 5)

static i2s_chan_handle_t rx_handle = nullptr;

// DMA receive-queue overflows: the capture task fell behind and the driver
// dropped a DMA buffer (~5.3 ms of audio at 256 frames / 48 kHz).
static volatile uint32_t dma_overflows = 0;

static bool IRAM_ATTR on_recv_overflow(i2s_chan_handle_t, i2s_event_data_t *, void *)
{
    dma_overflows = dma_overflows + 1;
    return false;
}

void audio_source_init()
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = PCM_DMA_DESC;
    chan_cfg.dma_frame_num = PCM_DMA_FRAMES;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle));

#if PCM_MCLK_INTEGER_DIV && !SOC_I2S_SUPPORTS_APLL
    // 160 MHz / 13 = 12.3077 MHz exactly, no fractional divider jitter.
    // Fs = 48077 Hz: 0.16% off, negligible for the analysis.
    const uint32_t i2s_rate = 48077;
#else
    const uint32_t i2s_rate = AUDIO_SAMPLE_RATE;
#endif
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(i2s_rate);
    // PCM1808 slave: MCLK = 256 fs, BCK = 64 fs (32-bit slots).
    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
#if SOC_I2S_SUPPORTS_APLL
    // ESP32-S2 has an audio PLL: exact 12.288 MHz instead of a fractional divide.
    clk_cfg.clk_src = I2S_CLK_SRC_APLL;
#endif

    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)PCM_MCLK_GPIO,
            .bclk = (gpio_num_t)PCM_BCK_GPIO,
            .ws   = (gpio_num_t)PCM_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)PCM_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = PCM_BCK_INVERT,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));

    i2s_event_callbacks_t cbs = {};
    cbs.on_recv_q_ovf = on_recv_overflow;
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(rx_handle, &cbs, nullptr));

    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    ESP_LOGI(TAG, "I2S started: Fs=%" PRIu32 " Hz, MCLK=%" PRIu32 " Hz%s, BCK=%" PRIu32 " Hz%s, canal %s",
             i2s_rate, i2s_rate * 256, i2s_rate != AUDIO_SAMPLE_RATE ? " (divisor inteiro)" : "",
             i2s_rate * 64, PCM_BCK_INVERT ? " (invertido)" : "", PCM_CHANNEL ? "RIN" : "LIN");
    ESP_LOGI(TAG, "MCLK GPIO=%d, BCK GPIO=%d, WS GPIO=%d, DIN GPIO=%d",
             PCM_MCLK_GPIO, PCM_BCK_GPIO, PCM_WS_GPIO, PCM_DIN_GPIO);

    static int32_t discard[PCM_DMA_FRAMES * 2];
    size_t bytes;
    const int64_t frames = (int64_t)AUDIO_SAMPLE_RATE * PCM_STARTUP_DISCARD_MS / 1000;
    for (int64_t got = 0; got < frames; got += bytes / (2 * sizeof(int32_t)))
        i2s_channel_read(rx_handle, discard, sizeof(discard), &bytes, portMAX_DELAY);
}

#if PCM_GLITCH_FIX
static inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

// Replaces the top byte of a 24-bit two's complement word and sign-extends.
static inline int32_t with_top(int32_t v, uint32_t top)
{
    const uint32_t u = ((uint32_t)v & ((1u << GLITCH_LOW_BITS) - 1)) | (top << GLITCH_LOW_BITS);
    return (int32_t)(u << 8) >> 8;
}

// Framing checks on the raw 32-bit slots, reported with the repair counts:
// the 8 padding bits under the 24-bit word must be 0, and the unused input
// (floating) reads exactly 0 - anything else means the word slipped.
static uint32_t pad_bad = 0, other_bad = 0;
static uint32_t pad_example = 0, other_example = 0;

// Symmetry: an AC signal is negative about half of the time. The PCM1808 fault
// forces the top bits to 1, turning positive samples negative, so a skewed
// ratio on the channel carrying audio measures how much of it is corrupted.
static uint32_t neg_count[2] = {0, 0}, nonzero_count[2] = {0, 0};

// Mutes: the PCM1808 forces DOUT to 0 while it resynchronises. Real 24-bit
// audio never holds exactly 0 for several samples, so such runs are dropouts.
#define MUTE_MIN_SAMPLES 8
static uint32_t zero_run = 0;
static uint32_t mute_events = 0, mute_samples = 0, mute_longest = 0;

static inline void track_mutes(int32_t x)
{
    if (x == 0) {
        zero_run++;
        return;
    }
    if (zero_run >= MUTE_MIN_SAMPLES) {
        mute_events++;
        mute_samples += zero_run;
        if (zero_run > mute_longest)
            mute_longest = zero_run;
    }
    zero_run = 0;
}

struct GlitchRecord {
    uint16_t pos;           // frame index inside the DMA read
    int32_t prev, mid, next;
    uint32_t raw;           // raw 32-bit slot of `mid`
};
#define GLITCH_RECORDS 6
static GlitchRecord records[GLITCH_RECORDS];
static int record_count = 0;

// 5 s snapshot, written by the capture task and printed by audio_source_log().
static struct {
    uint32_t exact_fixed, mean_fixed;
    uint32_t pad_bad, pad_example, other_bad, other_example;
    uint32_t mute_events, mute_samples, mute_longest;
    uint32_t neg_pct[2];
    int record_count;
    GlitchRecord records[GLITCH_RECORDS];
} report;
static volatile bool report_ready = false;

// Delays the stream by one sample so each sample can be judged against both
// neighbours. Every 5 s logs the repair counts, the framing checks and a few
// raw examples.
static int32_t repair(int32_t next, uint32_t next_raw, uint16_t next_pos)
{
    static int32_t prev = 0, mid = 0;
    static uint32_t mid_raw = 0;
    static uint16_t mid_pos = 0;
    static uint32_t exact_fixed = 0, mean_fixed = 0, seen = 0;

    // A corrupt sample is a lone spike: above both neighbours or below both.
    // A steep but monotonic run (loud square-ish audio) is real signal.
    const int32_t d_prev = mid - prev;
    const int32_t d_next = mid - next;
    const bool spike = (d_prev > GLITCH_THRESHOLD && d_next > GLITCH_THRESHOLD) ||
                       (d_prev < -GLITCH_THRESHOLD && d_next < -GLITCH_THRESHOLD);
    if (spike) {
        if (record_count < GLITCH_RECORDS)
            records[record_count++] = { mid_pos, prev, mid, next, mid_raw };

        const int32_t pred = (int32_t)(((int64_t)prev + next) / 2);
        int32_t best = mid;
        int32_t best_err = INT32_MAX;
        for (uint32_t top = 0; top < 256; top++) {
            const int32_t cand = with_top(mid, top);
            const int32_t err = iabs(cand - pred);
            if (err < best_err) {
                best_err = err;
                best = cand;
            }
        }
        if (best_err < GLITCH_EXACT_TOL) {
            mid = best;
            exact_fixed++;
        } else {
            mid = pred;
            mean_fixed++;
        }
    }

    const int32_t out = mid;
    prev = mid;
    mid = next;
    mid_raw = next_raw;
    mid_pos = next_pos;

    if (++seen >= GLITCH_REPORT_SAMPLES) {
        // Hand the counters to audio_source_log(): printing from the capture
        // task stalled it long enough to overflow the I2S DMA queue.
        if (!report_ready) {
            report.exact_fixed = exact_fixed;
            report.mean_fixed = mean_fixed;
            report.pad_bad = pad_bad;
            report.pad_example = pad_example;
            report.other_bad = other_bad;
            report.other_example = other_example;
            report.mute_events = mute_events;
            report.mute_samples = mute_samples;
            report.mute_longest = mute_longest;
            for (int ch = 0; ch < 2; ch++) {
                report.neg_pct[ch] = nonzero_count[ch] ? 100 * neg_count[ch] / nonzero_count[ch] : 0;
                neg_count[ch] = nonzero_count[ch] = 0;
            }
            report.record_count = record_count;
            memcpy(report.records, records, sizeof(records));
            report_ready = true;
        }
        mute_events = mute_samples = mute_longest = 0;
        record_count = 0;
        pad_bad = other_bad = 0;
        exact_fixed = mean_fixed = 0;
        seen = 0;
    }
    return out;
}
#endif

void audio_source_log()
{
#if PCM_GLITCH_FIX
    if (!report_ready)
        return;
    ESP_LOGW(TAG, "5 s: %" PRIu32 " amostras corrigidas (topo reposto: %" PRIu32 ", media: %" PRIu32 ")",
             report.exact_fixed + report.mean_fixed, report.exact_fixed, report.mean_fixed);
    ESP_LOGW(TAG, "     padding!=0: %" PRIu32 " (ex %08" PRIX32 ")  canal livre!=0: %" PRIu32 " (ex %08" PRIX32 ")",
             report.pad_bad, report.pad_example, report.other_bad, report.other_example);
    ESP_LOGW(TAG, "     silenciamentos (>=%d zeros): %" PRIu32 ", total %.1f ms, maior %.1f ms",
             MUTE_MIN_SAMPLES, report.mute_events, report.mute_samples * 1000.0f / AUDIO_SAMPLE_RATE,
             report.mute_longest * 1000.0f / AUDIO_SAMPLE_RATE);
    ESP_LOGW(TAG, "     amostras negativas: LIN %" PRIu32 "%%  RIN %" PRIu32 "%%  (sinal simetrico ~50%%)",
             report.neg_pct[0], report.neg_pct[1]);
    const uint32_t ovf = dma_overflows;
    dma_overflows = 0;
    ESP_LOGW(TAG, "     DMA overflow (audio perdido): %" PRIu32 " buffers, ~%.0f ms",
             ovf, ovf * PCM_DMA_FRAMES * 1000.0f / AUDIO_SAMPLE_RATE);
    for (int r = 0; r < report.record_count; r++) {
        const GlitchRecord &g = report.records[r];
        ESP_LOGW(TAG, "     pos %3u: %8" PRId32 " %8" PRId32 " %8" PRId32 "  (%06" PRIX32 " %06" PRIX32 " %06" PRIX32 ")",
                 g.pos, g.prev, g.mid, g.next,
                 (uint32_t)g.prev & 0xFFFFFFu, (uint32_t)g.mid & 0xFFFFFFu, (uint32_t)g.next & 0xFFFFFFu);
    }
    report_ready = false;
#endif
}

size_t audio_source_read(int32_t *out, size_t max)
{
    static int32_t frames[PCM_DMA_FRAMES * 2];
    size_t bytes = 0;

    esp_err_t err = i2s_channel_read(rx_handle, frames, sizeof(frames), &bytes, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S read: %s", esp_err_to_name(err));
        return 0;
    }

    size_t n = bytes / (2 * sizeof(int32_t));
    if (n > max)
        n = max;
    for (size_t i = 0; i < n; i++) {
        // 24-bit audio left-aligned in the 32-bit slot.
        const uint32_t raw = (uint32_t)frames[2 * i + PCM_CHANNEL];
        const int32_t x = (int32_t)raw >> 8;
#if PCM_GLITCH_FIX
        const uint32_t other = (uint32_t)frames[2 * i + (1 - PCM_CHANNEL)];
        if (raw & 0xFFu) {
            pad_bad++;
            pad_example = raw;
        }
        if (other) {
            other_bad++;
            other_example = other;
        }
        track_mutes(x);
        for (int ch = 0; ch < 2; ch++) {
            const int32_t v = frames[2 * i + ch] >> 8;
            if (v != 0) {
                nonzero_count[ch]++;
                neg_count[ch] += v < 0;
            }
        }
        out[i] = repair(x, raw, (uint16_t)i);
#else
        out[i] = x;
#endif
    }
    return n;
}

#endif // AUDIO_SOURCE == AUDIO_SRC_PCM1808
