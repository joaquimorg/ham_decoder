#include "qrss.h"

#pragma GCC optimize("O2")

#include <math.h>
#include <string.h>

#include "fft.h"
#include "ui_hub.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;
// Two decimation stages of 8 (12 kHz -> 1500 Hz -> 187.5 Hz), windowed-sinc
// low-pass FIRs. The view keeps +-47 Hz; the second stage passes that and
// stops what would alias into it (above 187.5 - 47 = 140 Hz).
static_assert(QRSS_DECIM == 64, "two stages of 8");
constexpr int D1 = 8, D2 = 8;
constexpr int TAPS1 = 32, TAPS2 = 64;
constexpr float CUT1_HZ = 250.0f, CUT2_HZ = 70.0f;

constexpr int NFFT = QRSS_FFT;
constexpr int HOP = NFFT / 2;
static_assert(UI_QRSS_BINS <= NFFT, "the view is the middle of the FFT");

float h1[TAPS1], h2[TAPS2];
float d1r[TAPS1], d1i[TAPS1], d2r[TAPS2], d2i[TAPS2];
int p1 = 0, p2 = 0, n1 = 0, n2 = 0;

float center = QRSS_DEFAULT_HZ;
float osc_r = 1.0f, osc_i = 0.0f, rot_r = 1.0f, rot_i = 0.0f;

EXT_RAM_BSS_ATTR float buf_r[NFFT], buf_i[NFFT];    // last NFFT baseband samples
int fill = 0;
EXT_RAM_BSS_ATTR float fr[NFFT], fi[NFFT];
EXT_RAM_BSS_ATTR float win[NFFT];
ComplexFft fft;

void design(float *h, int taps, float cut_hz, float fs)
{
    const float fc = cut_hz / fs;
    float sum = 0.0f;
    for (int k = 0; k < taps; k++) {
        const float m = k - (taps - 1) / 2.0f;
        const float sinc = m == 0.0f ? 2.0f * fc : sinf(2.0f * (float)M_PI * fc * m) / ((float)M_PI * m);
        const float w = 0.42f - 0.5f * cosf(2.0f * (float)M_PI * k / (taps - 1)) +
                        0.08f * cosf(4.0f * (float)M_PI * k / (taps - 1));    // Blackman
        h[k] = sinc * w;
        sum += h[k];
    }
    for (int k = 0; k < taps; k++)
        h[k] /= sum;
}

// Dot product of a circular delay line (newest at pos - 1) with the taps.
inline void fir(const float *h, int taps, const float *dr, const float *di, int pos, float *yr, float *yi)
{
    float ar = 0.0f, ai = 0.0f;
    int j = pos;
    for (int k = 0; k < taps; k++) {
        if (--j < 0)
            j = taps - 1;
        ar += h[k] * dr[j];
        ai += h[k] * di[j];
    }
    *yr = ar;
    *yi = ai;
}

void column()
{
    for (int k = 0; k < NFFT; k++) {
        fr[k] = buf_r[k] * win[k];
        fi[k] = buf_i[k] * win[k];
    }
    fft.run(fr, fi);
    uint8_t col[UI_QRSS_BINS];
    // Hann window and the mixer: a full-scale tone gives |X| ~ NFFT / 4.
    const float norm = 16.0f / ((float)NFFT * NFFT);
    for (int i = 0; i < UI_QRSS_BINS; i++) {
        const int k = (i - UI_QRSS_BINS / 2 + NFFT) % NFFT;
        const float p = (fr[k] * fr[k] + fi[k] * fi[k]) * norm;
        const float q = (10.0f * log10f(p + 1e-16f) + 140.0f) * 2.0f;
        col[i] = q <= 0.0f ? 0 : q >= 255.0f ? 255 : (uint8_t)q;
    }
    ui_push_qrss_column(col);
}

}    // namespace

void qrss_init()
{
    design(h1, TAPS1, CUT1_HZ, FS);
    design(h2, TAPS2, CUT2_HZ, FS / D1);
    for (int k = 0; k < NFFT; k++)
        win[k] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * k / NFFT);
    fft.init(NFFT);
    qrss_set_center(QRSS_DEFAULT_HZ);
}

void qrss_set_center(float hz)
{
    if (hz < 300.0f) hz = 300.0f;
    if (hz > 3000.0f) hz = 3000.0f;
    center = hz;
    const float w = 2.0f * (float)M_PI * hz / FS;
    rot_r = cosf(w);
    rot_i = -sinf(w);
}

float qrss_center() { return center; }
float qrss_bin_hz() { return (float)QRSS_RATE / NFFT; }

void qrss_process(const float *x, int n)
{
    for (int i = 0; i < n; i++) {
        d1r[p1] = x[i] * osc_r;
        d1i[p1] = x[i] * osc_i;
        if (++p1 == TAPS1)
            p1 = 0;
        const float r = osc_r * rot_r - osc_i * rot_i;
        osc_i = osc_r * rot_i + osc_i * rot_r;
        osc_r = r;
        if (++n1 < D1)
            continue;
        n1 = 0;
        fir(h1, TAPS1, d1r, d1i, p1, &d2r[p2], &d2i[p2]);
        if (++p2 == TAPS2)
            p2 = 0;
        if (++n2 < D2)
            continue;
        n2 = 0;
        float yr, yi;
        fir(h2, TAPS2, d2r, d2i, p2, &yr, &yi);
        buf_r[fill] = yr;
        buf_i[fill] = yi;
        if (++fill == NFFT) {
            column();
            memmove(buf_r, buf_r + HOP, (NFFT - HOP) * sizeof(float));
            memmove(buf_i, buf_i + HOP, (NFFT - HOP) * sizeof(float));
            fill = NFFT - HOP;
        }
    }
    const float g = 1.0f / sqrtf(osc_r * osc_r + osc_i * osc_i);    // keep |osc| = 1
    osc_r *= g;
    osc_i *= g;
}
