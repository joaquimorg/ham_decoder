#include "fm_demod.h"

#include <math.h>

#include "config.h"

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;
constexpr int TAPS = FM_TAPS;

float taps[TAPS];
float hist_r[2 * TAPS], hist_i[2 * TAPS];    // doubled ring: contiguous window
int hist_pos = 0;
float osc_r = 1.0f, osc_i = 0.0f, rot_r, rot_i;
float prev_r = 1.0f, prev_i = 0.0f;
int renorm = 0;

} // namespace

void fm_demod_init()
{
    // Hamming-windowed sinc low-pass, unity gain at DC.
    const float fc = FM_CUTOFF_HZ / FS;
    float sum = 0.0f;
    for (int k = 0; k < TAPS; k++) {
        const float m = k - (TAPS - 1) / 2.0f;
        const float sinc = m == 0.0f ? 2.0f * fc : sinf(2.0f * (float)M_PI * fc * m) / ((float)M_PI * m);
        taps[k] = sinc * (0.54f - 0.46f * cosf(2.0f * (float)M_PI * k / (TAPS - 1)));
        sum += taps[k];
    }
    for (int k = 0; k < TAPS; k++)
        taps[k] /= sum;
    const float w = -2.0f * (float)M_PI * FM_CENTER_HZ / FS;
    rot_r = cosf(w);
    rot_i = sinf(w);
}

void fm_demod_process(const float *x, float *hz, int n, float *mag)
{
    constexpr float SCALE = FS / (2.0f * (float)M_PI);
    for (int i = 0; i < n; i++) {
        // Mix the centre frequency down to 0 Hz and low-pass.
        hist_r[hist_pos] = hist_r[hist_pos + TAPS] = x[i] * osc_r;
        hist_i[hist_pos] = hist_i[hist_pos + TAPS] = x[i] * osc_i;
        if (++hist_pos == TAPS)
            hist_pos = 0;
        const float t = osc_r * rot_r - osc_i * rot_i;
        osc_i = osc_r * rot_i + osc_i * rot_r;
        osc_r = t;
        if (++renorm == 1024) {
            renorm = 0;
            const float g = 1.0f / sqrtf(osc_r * osc_r + osc_i * osc_i);
            osc_r *= g;
            osc_i *= g;
        }
        float zr = 0.0f, zi = 0.0f;
        const float *hr = hist_r + hist_pos, *hi = hist_i + hist_pos;
        for (int k = 0; k < TAPS; k++) {
            zr += taps[k] * hr[k];
            zi += taps[k] * hi[k];
        }
        if (mag)
            mag[i] = sqrtf(zr * zr + zi * zi);
        // Phase step between samples = frequency offset from the centre.
        const float dr = zr * prev_r + zi * prev_i;
        const float di = zi * prev_r - zr * prev_i;
        prev_r = zr;
        prev_i = zi;
        hz[i] = FM_CENTER_HZ + atan2f(di, dr) * SCALE;
    }
}
