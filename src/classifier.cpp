#include <math.h>

// The project builds with -Og; this DSP runs on every sample of the analysis.
#pragma GCC optimize("O2")
#include <string.h>
#include <stdlib.h>

#include "classifier.h"
#include "config.h"
#include "ml_model.h"

// Mirror of tools/ml/features.py: keep both in sync (same constants, same order).

namespace {

constexpr int HALF = FFT_SIZE / 2;
constexpr float BIN_HZ = (float)DSP_SAMPLE_RATE / FFT_SIZE;
constexpr int KMIN = 9;     // ceil(100 / BIN_HZ)
constexpr int KMAX = 298;   // floor(3500 / BIN_HZ)

constexpr int MOD_LEN = 100;
constexpr int MOD_BINS = 40;
constexpr int LOCAL_BANDS = 48;
constexpr int LOCAL_W = 3;
constexpr int WIDE_BANDS = 16;
constexpr float WIDE_HZ0 = 200.0f, WIDE_HZ = 200.0f;
constexpr int N_FEATURES = LOCAL_BANDS + WIDE_BANDS + MOD_BINS + 6;
static_assert(N_FEATURES == ML_N_FEATURES, "features.py and classifier.cpp disagree");

float rel[HALF];
float sorted_buf[HALF];
float feat[N_FEATURES];
float mod_cos[MOD_BINS][MOD_LEN];
float mod_sin[MOD_BINS][MOD_LEN];
bool tables = false;

int cmp_float(const void *a, const void *b)
{
    const float fa = *(const float *)a;
    const float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

float pctl(const float *v, int n, float p)
{
    memcpy(sorted_buf, v, n * sizeof(float));
    qsort(sorted_buf, n, sizeof(float), cmp_float);
    return sorted_buf[(int)(p * (n - 1) + 0.5f)];
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void extract(const float *psd, const float *env_db, int env_count)
{
    if (!tables) {
        for (int m = 0; m < MOD_BINS; m++) {
            for (int t = 0; t < MOD_LEN; t++) {
                const float w = 2.0f * (float)M_PI * (m + 1) * t / MOD_LEN;
                mod_cos[m][t] = cosf(w);
                mod_sin[m][t] = sinf(w);
            }
        }
        tables = true;
    }

    for (int k = 0; k < HALF; k++)
        rel[k] = 10.0f * log10f(psd[k] + 1e-14f);
    const float floor_db = pctl(&rel[KMIN], KMAX - KMIN + 1, 0.5f);
    int kp = KMIN;
    for (int k = 0; k < HALF; k++) {
        rel[k] -= floor_db;
        if (k >= KMIN && k <= KMAX && rel[k] > rel[kp])
            kp = k;
    }
    const float snr = rel[kp];

    int i = 0;
    for (int b = 0; b < LOCAL_BANDS; b++) {
        const int c = kp + (b - LOCAL_BANDS / 2) * LOCAL_W;
        float m = 0.0f;
        for (int k = c - 1; k <= c + 1; k++) {
            if (k >= 0 && k < HALF && rel[k] > m)
                m = rel[k];
        }
        feat[i++] = clampf(m / 40.0f, 0.0f, 1.5f);
    }
    for (int b = 0; b < WIDE_BANDS; b++) {
        const int k0 = (int)((WIDE_HZ0 + b * WIDE_HZ) / BIN_HZ);
        const int k1 = (int)((WIDE_HZ0 + (b + 1) * WIDE_HZ) / BIN_HZ);
        float s = 0.0f;
        for (int k = k0; k < k1; k++)
            s += rel[k];
        feat[i++] = clampf(s / (k1 - k0) / 20.0f, 0.0f, 1.5f);
    }

    // Envelope modulation spectrum (1..40 Hz) over 1 s of 10 ms blocks.
    const int n = env_count < MOD_LEN ? env_count : MOD_LEN;
    float env_max = -1e9f, mean_db = 0.0f;
    for (int t = 0; t < n; t++) {
        env_max = fmaxf(env_max, env_db[t]);
        mean_db += env_db[t];
    }
    mean_db /= n > 0 ? n : 1;
    float var_db = 0.0f;
    float lin[MOD_LEN];
    float lin_mean = 0.0f;
    for (int t = 0; t < n; t++) {
        var_db += (env_db[t] - mean_db) * (env_db[t] - mean_db);
        lin[t] = powf(10.0f, (env_db[t] - env_max) / 20.0f);
        lin_mean += lin[t];
    }
    lin_mean /= n > 0 ? n : 1;
    const float rng = n > 0 ? pctl(env_db, n, 0.9f) - pctl(env_db, n, 0.1f) : 0.0f;

    float mags[MOD_BINS];
    float tot = 1e-9f;
    for (int m = 0; m < MOD_BINS; m++) {
        float sr = 0.0f, si = 0.0f;
        for (int t = 0; t < n; t++) {
            const float v = lin[t] - lin_mean;
            sr += v * mod_cos[m][t];
            si += v * mod_sin[m][t];
        }
        mags[m] = sqrtf(sr * sr + si * si);
        tot += mags[m];
    }
    for (int m = 0; m < MOD_BINS; m++)
        feat[i++] = mags[m] / tot * 4.0f;

    int occ = 0;
    double log_sum = 0.0, lin_sum = 0.0;
    for (int k = KMIN; k <= KMAX; k++) {
        if (rel[k] >= 10.0f)
            occ++;
        const double p = psd[k] + 1e-14;
        log_sum += log(p);
        lin_sum += p;
    }
    const int nb = KMAX - KMIN + 1;
    feat[i++] = fminf(snr / 60.0f, 1.5f);
    feat[i++] = fminf(rng / 30.0f, 1.5f);
    feat[i++] = (float)occ / nb * 4.0f;
    feat[i++] = (float)(exp(log_sum / nb) / (lin_sum / nb));
    feat[i++] = kp * BIN_HZ / 3500.0f;
    feat[i++] = fminf(sqrtf(var_db / (n > 0 ? n : 1)) / 15.0f, 1.5f);
}

void dense(const float *in, int nin, const int8_t *w, float scale, const float *b,
           float *out, int nout, bool relu)
{
    for (int o = 0; o < nout; o++)
        out[o] = 0.0f;
    for (int j = 0; j < nin; j++) {
        const float x = in[j];
        if (x == 0.0f)
            continue;
        const int8_t *row = &w[j * nout];
        for (int o = 0; o < nout; o++)
            out[o] += x * row[o];
    }
    for (int o = 0; o < nout; o++) {
        out[o] = out[o] * scale + b[o];
        if (relu && out[o] < 0.0f)
            out[o] = 0.0f;
    }
}

} // namespace

MlResult classifier_run(const float *psd, const float *env_db, int env_count)
{
    extract(psd, env_db, env_count);

    float h1[ML_H1], h2[ML_H2], z[ML_N_CLASSES];
    dense(feat, ML_N_FEATURES, ML_W1, ML_W1_SCALE, ML_B1, h1, ML_H1, true);
    dense(h1, ML_H1, ML_W2, ML_W2_SCALE, ML_B2, h2, ML_H2, true);
    dense(h2, ML_H2, ML_W3, ML_W3_SCALE, ML_B3, z, ML_N_CLASSES, false);

    int best = 0;
    float zmax = z[0];
    for (int c = 1; c < ML_N_CLASSES; c++) {
        if (z[c] > zmax) {
            zmax = z[c];
            best = c;
        }
    }
    float sum = 0.0f;
    for (int c = 0; c < ML_N_CLASSES; c++)
        sum += expf(z[c] - zmax);
    return { best, 1.0f / sum };
}

const char *ml_class_name(int cls)
{
    return cls >= 0 && cls < ML_N_CLASSES ? ML_CLASS_NAMES[cls] : "?";
}

const float *classifier_features(int *n)
{
    *n = N_FEATURES;
    return feat;
}
