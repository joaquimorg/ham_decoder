#include "noise_reduce.h"

// The project builds with -Og; the per-sample DSP here needs real optimisation
// (at 48 kHz the STFT took ~3 ms of each 5.3 ms block and starved the analysis).
#pragma GCC optimize("O2")

#if MONITOR_NR

#include <math.h>
#include <string.h>

#include "esp_attr.h"

namespace {

constexpr int N = 128;
constexpr int H = N / 2;
constexpr int HALF = N / 2 + 1;
constexpr float BIN_HZ = (float)NR_RATE / N;    // 93.75 Hz

// Buffers in PSRAM (EXT_RAM_BSS_ATTR): the internal RAM is needed by the Wi-Fi
// and the audio DMA; the working set fits the data cache.

// Voice band whose bins define the noise floor.
constexpr int BAND_LO = (int)(150.0f / BIN_HZ + 0.5f);
constexpr int BAND_HI = (int)(4000.0f / BIN_HZ + 0.5f);
constexpr int BAND_N = BAND_HI - BAND_LO + 1;
constexpr int PERCENTILE_IDX = BAND_N / 4;

// Mean of exponentially distributed noise power = percentile-25 / 0.288;
// a little under that, so speech is not eaten.
constexpr float PCT_TO_MEAN = 3.0f;
constexpr float OVERSUB = 1.5f;

EXT_RAM_BSS_ATTR float win[H];             // sqrt-Hann, first half (symmetric)
EXT_RAM_BSS_ATTR float tw_c[H];
EXT_RAM_BSS_ATTR float tw_s[H];
EXT_RAM_BSS_ATTR uint16_t bitrev[N];
EXT_RAM_BSS_ATTR float re[N];
EXT_RAM_BSS_ATTR float im[N];
EXT_RAM_BSS_ATTR float hist[N];            // last N input samples
EXT_RAM_BSS_ATTR float ola[N];             // overlap-add accumulator
EXT_RAM_BSS_ATTR float outbuf[H];
EXT_RAM_BSS_ATTR float gain_prev[HALF];
float floor_pw = -1.0f;
float gain_min = 0.15f;
int idx = 0;

inline float w_at(int i) { return i < H ? win[i] : win[N - 1 - i]; }

// In-place radix-2 complex FFT (forward, e^-j).
void fft()
{
    for (int i = 0; i < N; i++) {
        const int j = bitrev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        const int half = len >> 1, step = N / len;
        for (int i = 0; i < N; i += len) {
            for (int k = 0; k < half; k++) {
                const float wr = tw_c[k * step], wi = -tw_s[k * step];
                const int a = i + k, b = a + half;
                const float xr = re[b] * wr - im[b] * wi;
                const float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

void process_frame()
{
    for (int i = 0; i < N; i++) {
        re[i] = hist[i] * w_at(i);
        im[i] = 0.0f;
    }
    fft();

    EXT_RAM_BSS_ATTR static float p[HALF];
    EXT_RAM_BSS_ATTR static float ps[HALF];
    for (int k = 0; k < HALF; k++)
        p[k] = re[k] * re[k] + im[k] * im[k];

    // Noise floor: low percentile of the band bins, quick to fall, slow to rise.
    float tmp[BAND_N];
    memcpy(tmp, &p[BAND_LO], sizeof(tmp));
    for (int i = 1; i < BAND_N; i++) {    // insertion sort, ~40 elements
        const float v = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > v) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = v;
    }
    const float q = tmp[PERCENTILE_IDX];
    if (floor_pw < 0.0f)
        floor_pw = q;
    else
        floor_pw += (q < floor_pw ? 0.3f : 0.02f) * (q - floor_pw);
    const float noise = floor_pw * PCT_TO_MEAN * OVERSUB;

    // Frequency smoothing of the power, then a Wiener-like gain smoothed in time.
    for (int k = 0; k < HALF; k++) {
        const float a = p[k > 0 ? k - 1 : 0], b = p[k < HALF - 1 ? k + 1 : HALF - 1];
        ps[k] = 0.5f * p[k] + 0.25f * (a + b);
    }
    for (int k = 0; k < HALF; k++) {
        float g = ps[k] > 1e-12f ? 1.0f - noise / ps[k] : gain_min;
        if (g < gain_min)
            g = gain_min;
        g = 0.5f * gain_prev[k] + 0.5f * g;
        gain_prev[k] = g;
        re[k] *= g;
        im[k] *= g;
        if (k > 0 && k < N / 2) {
            re[N - k] = re[k];
            im[N - k] = -im[k];
        }
    }

    // Inverse FFT through conjugation: ifft(X) = conj(fft(conj(X))) / N.
    for (int i = 0; i < N; i++)
        im[i] = -im[i];
    fft();
    const float scale = 1.0f / N;
    for (int i = 0; i < N; i++)
        ola[i] += re[i] * scale * w_at(i);

    memcpy(outbuf, ola, sizeof(outbuf));
    memmove(ola, ola + H, H * sizeof(float));
    memset(ola + H, 0, H * sizeof(float));
    memmove(hist, hist + H, H * sizeof(float));
}

}    // namespace

void nr_init()
{
    for (int i = 0; i < H; i++) {
        win[i] = sinf((float)M_PI * (i + 0.5f) / N);
        tw_c[i] = cosf(2.0f * (float)M_PI * i / N);
        tw_s[i] = sinf(2.0f * (float)M_PI * i / N);
    }
    for (int i = 0; i < N; i++) {
        int r = 0;
        for (int b = 0, v = i; b < 7; b++, v >>= 1)
            r = (r << 1) | (v & 1);
        bitrev[i] = (uint16_t)r;
    }
    for (int k = 0; k < HALF; k++)
        gain_prev[k] = 1.0f;
    memset(hist, 0, sizeof(hist));
    memset(ola, 0, sizeof(ola));
    memset(outbuf, 0, sizeof(outbuf));
    floor_pw = -1.0f;
    idx = 0;
    gain_min = powf(10.0f, -MONITOR_NR_DEPTH_DB / 20.0f);
}

float nr_process(float x)
{
    const float o = outbuf[idx];
    hist[H + idx] = x;
    if (++idx == H) {
        idx = 0;
        process_frame();
    }
    return o;
}

#endif
