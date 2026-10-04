#include "audio_monitor.h"

// The project builds with -Og; the per-sample DSP here needs real optimisation
// (the STFT took ~3 ms of each 5.3 ms block and starved the analysis).
#pragma GCC optimize("O2")

#if AUDIO_MONITOR

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "es8311.h"
#include "noise_reduce.h"

#include <math.h>

static const char *TAG = "MONITOR";

// Speaker band-pass: 4th-order Butterworth high-pass and low-pass.
struct Biquad {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
    float run(float x)
    {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

static Biquad hp[2];         // MONITOR_LOW_HZ, 4th-order Butterworth
static Biquad lp[2];         // MONITOR_HIGH_HZ, 4th-order Butterworth
static bool has_hp = false, has_lp = false;

static void biquad_design(Biquad &q, bool high, float hz, float Q)
{
    const float w0 = 2.0f * (float)M_PI * hz / AUDIO_SAMPLE_RATE;
    const float c = cosf(w0), alpha = sinf(w0) / (2.0f * Q), a0 = 1.0f + alpha;
    q = {};
    q.b0 = (high ? (1.0f + c) : (1.0f - c)) / 2.0f / a0;
    q.b1 = (high ? -(1.0f + c) : (1.0f - c)) / a0;
    q.b2 = q.b0;
    q.a1 = -2.0f * c / a0;
    q.a2 = (1.0f - alpha) / a0;
}

static void band_init()
{
#if MONITOR_NR
    nr_init();
#endif
#if MONITOR_LOW_HZ > 0
    biquad_design(hp[0], true, MONITOR_LOW_HZ, 0.5412f);
    biquad_design(hp[1], true, MONITOR_LOW_HZ, 1.3066f);
    has_hp = true;
#endif
#if MONITOR_HIGH_HZ > 0
    biquad_design(lp[0], false, MONITOR_HIGH_HZ, 0.5412f);
    biquad_design(lp[1], false, MONITOR_HIGH_HZ, 1.3066f);
    has_lp = true;
#endif
}

#if MONITOR_GATE
static float gate_env = 0.0f, gate_gain = 0.0f;
static int gate_hold = 0;
static bool gate_open = false;
static volatile int gate_event = 0;    // 1 opened, 2 closed; reported by the task
static volatile float gate_event_env = 0.0f;

static inline float gate(float v)
{
    // Per-sample coefficients written for 48 kHz, scaled to the actual rate.
    constexpr float k = 48000.0f / AUDIO_SAMPLE_RATE;
    const float full = 8388608.0f;
    static const float open_thr = full * powf(10.0f, MONITOR_GATE_OPEN / 20.0f);
    static const float close_thr = full * powf(10.0f, MONITOR_GATE_CLOSE / 20.0f);
    const float a = fabsf(v);
    gate_env += (a > gate_env ? 0.02f * k : 0.0005f * k) * (a - gate_env);
    if (gate_env > open_thr) {
        if (!gate_open) {
            gate_event = 1;
            gate_event_env = gate_env;
        }
        gate_open = true;
        gate_hold = AUDIO_SAMPLE_RATE * 150 / 1000;
    } else if (gate_env > close_thr) {
        if (gate_open)
            gate_hold = AUDIO_SAMPLE_RATE * 150 / 1000;
    } else if (gate_open && --gate_hold <= 0) {
        gate_open = false;
        gate_event = 2;
        gate_event_env = gate_env;
    }
    gate_gain += ((gate_open ? 1.0f : 0.0f) - gate_gain) * (0.002f * k);    // ~10 ms ramp
    return v * gate_gain;
}
#endif

static inline int32_t band(int32_t x)
{
    float v = (float)x;
    if (has_hp)
        v = hp[1].run(hp[0].run(v));
#if MONITOR_NR
    // The noise reduction runs at NR_RATE: with a 48 kHz capture, a 4-sample
    // mean in and the held output back at the full rate (the low-pass removes
    // the images); with the ES8311 at 12 kHz, NR_DECIM is 1.
    static float acc = 0.0f, held = 0.0f;
    static int phase = 0;
    acc += v;
    if (++phase == NR_DECIM) {
        held = nr_process(acc * (1.0f / NR_DECIM));
        acc = 0.0f;
        phase = 0;
    }
    v = held;
#endif
    if (has_lp)
        v = lp[1].run(lp[0].run(v));
#if MONITOR_GATE
    v = gate(v);
#endif
    if (v > 8388607.0f)
        v = 8388607.0f;
    else if (v < -8388608.0f)
        v = -8388608.0f;
    return (int32_t)v;
}

#define MON_BLOCK       128                 // samples per I2S write
#define MON_BUFFER      (16 * MON_BLOCK)    // ~43 ms of queued audio at most

static StreamBufferHandle_t queue = nullptr;
static volatile bool running = false;       // volume > 0 and the output task is up
static int volume = MONITOR_DEFAULT_VOL;

// The queue decouples the capture task from the I2S output, which with the
// internal ADC runs on another clock: when it fills up, samples are dropped.
static void monitor_task(void *)
{
    static int32_t mono[MON_BLOCK];
    static int32_t stereo[MON_BLOCK * 2];

    while (true) {
        const size_t bytes = xStreamBufferReceive(queue, mono, sizeof(mono), pdMS_TO_TICKS(50));
        const size_t n = bytes / sizeof(int32_t);
        if (n == 0)
            continue;    // nothing queued (monitor off): the DMA sends zeros
        for (size_t i = 0; i < n; i++)
            stereo[2 * i] = stereo[2 * i + 1] = band(mono[i]) << 8;    // 24 bits left-aligned
        size_t written;
        i2s_channel_write(es8311_tx(), stereo, n * 2 * sizeof(int32_t), &written, pdMS_TO_TICKS(100));
    }
}

void audio_monitor_init()
{
    band_init();
    es8311_start();
    es8311_set_volume(volume);
#if AUDIO_SOURCE == AUDIO_SRC_ES8311
    // Capture and output share the I2S clocks: no queue needed (see audio_monitor_write).
    running = volume > 0;
    return;
#endif
    // In PSRAM: the internal RAM is needed by the Wi-Fi buffers.
    static StaticStreamBuffer_t queue_struct;
    uint8_t *storage = (uint8_t *)heap_caps_malloc(MON_BUFFER * sizeof(int32_t) + 1, MALLOC_CAP_SPIRAM);
    if (storage)
        queue = xStreamBufferCreateStatic(MON_BUFFER * sizeof(int32_t), MON_BLOCK * sizeof(int32_t),
                                          storage, &queue_struct);
    if (!queue) {
        ESP_LOGE(TAG, "sem memoria para a fila de audio");
        return;
    }
    xTaskCreatePinnedToCore(monitor_task, "monitor_task", 3072, nullptr, 5, nullptr, 1);
    running = volume > 0;
}

void audio_monitor_write(const int32_t *samples, size_t n)
{
    if (!running)
        return;
#if MONITOR_GATE
    if (gate_event) {
        const int ev = gate_event;
        gate_event = 0;
        ESP_LOGI(TAG, "porta %s (nivel %.0f dBFS)", ev == 1 ? "aberta" : "fechada",
                 20.0f * log10f(gate_event_env / 8388608.0f + 1e-9f));
    }
#endif
#if AUDIO_SOURCE == AUDIO_SRC_ES8311
    // Same clock for RX and TX: writing each captured block straight to the DAC
    // keeps the two in lock-step, so nothing is dropped or starved. A queue
    // between them dropped samples whenever it filled up (audible hiccups).
    static int32_t stereo[256 * 2];
    while (n) {
        const size_t m = n < 256 ? n : 256;
        for (size_t i = 0; i < m; i++)
            stereo[2 * i] = stereo[2 * i + 1] = band(samples[i]) << 8;
        size_t written;
        i2s_channel_write(es8311_tx(), stereo, m * 2 * sizeof(int32_t), &written, pdMS_TO_TICKS(20));
        samples += m;
        n -= m;
    }
    return;
#endif
    xStreamBufferSend(queue, samples, n * sizeof(int32_t), 0);
}

void audio_monitor_set_volume(int percent)
{
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (percent == volume)
        return;
    volume = percent;
    es8311_set_volume(volume);
#if AUDIO_SOURCE == AUDIO_SRC_ES8311
    running = volume > 0;
#else
    running = queue && volume > 0;
#endif
}

#endif
