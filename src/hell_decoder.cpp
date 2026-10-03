#include "hell_decoder.h"

// The project builds with -Og; this runs on every sample.
#pragma GCC optimize("O2")

#include <math.h>
#include <string.h>

#include "esp_attr.h"
#include "ui_hub.h"

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;
constexpr float PIX_RATE = 17.5f * HELL_ROWS;          // 245 pixels/s
constexpr int BOX = (int)(FS / PIX_RATE + 0.5f);      // one pixel: the low-pass
constexpr float AGC_ATTACK = 0.2f, AGC_DECAY = 0.002f;    // per pixel (~2 s)

float tone = 0.0f;
float osc_r = 1.0f, osc_i = 0.0f, rot_r = 1.0f, rot_i = 0.0f;
EXT_RAM_BSS_ATTR float ring_r[BOX], ring_i[BOX];
float acc_r = 0.0f, acc_i = 0.0f;
int pos = 0;
float pix_phase = 0.0f;
float peak = 0.0f, floor_lvl = 0.0f;
uint8_t col[HELL_ROWS];
int row = 0;

void (*sink)(const uint8_t *col) = ui_push_hell_column;

}    // namespace

void hell_init()
{
    hell_set_tone(0.0f);
}

void hell_set_tone(float hz)
{
    if (hz > 0.0f && fabsf(hz - tone) < 5.0f)
        return;    // small moves of the peak: keep going
    tone = hz;
    if (hz <= 0.0f)
        return;
    const float w = 2.0f * (float)M_PI * hz / FS;
    rot_r = cosf(w);
    rot_i = -sinf(w);
}

float hell_tone_hz() { return tone; }

void hell_process(const float *x, int n)
{
    if (tone <= 0.0f)
        return;
    for (int i = 0; i < n; i++) {
        const float r = x[i] * osc_r, im = x[i] * osc_i;
        const float t = osc_r * rot_r - osc_i * rot_i;
        osc_i = osc_r * rot_i + osc_i * rot_r;
        osc_r = t;
        acc_r += r - ring_r[pos];
        acc_i += im - ring_i[pos];
        ring_r[pos] = r;
        ring_i[pos] = im;
        if (++pos == BOX)
            pos = 0;
        pix_phase += PIX_RATE / FS;
        if (pix_phase < 1.0f)
            continue;
        pix_phase -= 1.0f;
        // One pixel: the tone level against its recent peak and floor.
        const float m = sqrtf(acc_r * acc_r + acc_i * acc_i);
        peak += (m > peak ? AGC_ATTACK : AGC_DECAY) * (m - peak);
        floor_lvl += (m < floor_lvl ? AGC_ATTACK : AGC_DECAY) * (m - floor_lvl);
        const float span = peak - floor_lvl;
        float v = span > 1e-9f ? (m - floor_lvl) / span : 0.0f;
        v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        col[row] = (uint8_t)(v * 255.0f);
        if (++row == HELL_ROWS) {
            row = 0;
            sink(col);
        }
    }
    const float g = 1.0f / sqrtf(osc_r * osc_r + osc_i * osc_i);
    osc_r *= g;
    osc_i *= g;
}

#if HELL_SELFTEST

#include "esp_log.h"

namespace {

constexpr int TEST_COLS = 140;    // 8 s
EXT_RAM_BSS_ATTR uint8_t got_cols[TEST_COLS + 32][HELL_ROWS];
int got_n = 0;
void capture(const uint8_t *c)
{
    if (got_n < TEST_COLS + 32)
        memcpy(got_cols[got_n++], c, HELL_ROWS);
}

}    // namespace

void hell_selftest()
{
    // A pseudo-random pixel pattern keyed on a 1000 Hz tone, with noise; the
    // decoded pixels must match it at some pixel offset (Hell has no sync).
    EXT_RAM_BSS_ATTR static uint8_t px[TEST_COLS * HELL_ROWS];
    uint32_t rnd = 77;
    for (int i = 0; i < TEST_COLS * HELL_ROWS; i++) {
        rnd = rnd * 1664525u + 1013904223u;
        px[i] = (rnd >> 28) & 1;
    }
    sink = capture;
    got_n = 0;
    hell_set_tone(0.0f);
    hell_set_tone(1000.0f);
    EXT_RAM_BSS_ATTR static float block[FFT_SIZE];
    int bpos = 0;
    float ph = 0.0f;
    const int total = (int)(TEST_COLS * HELL_ROWS * FS / PIX_RATE);
    for (int s = 0; s < total; s++) {
        const int p = (int)(s * PIX_RATE / FS);
        ph += 2.0f * (float)M_PI * 1000.0f / FS;
        if (ph > 2.0f * (float)M_PI)
            ph -= 2.0f * (float)M_PI;
        float nz = 0.0f;
        for (int j = 0; j < 4; j++) {
            rnd = rnd * 1664525u + 1013904223u;
            nz += (float)(int32_t)rnd / 2147483648.0f;
        }
        block[bpos++] = (px[p] ? 0.2f * sinf(ph) : 0.0f) + 0.02f * nz;
        if (bpos == FFT_SIZE || s == total - 1) {
            hell_process(block, bpos);
            bpos = 0;
        }
    }
    sink = ui_push_hell_column;
    // Pixel stream of the decoded columns against the pattern, best offset.
    const int npx = got_n * HELL_ROWS;
    int best = 0, best_lag = 0, compared = 0;
    for (int lag = -4; lag <= 4; lag++) {
        int ok = 0, n = 0;
        for (int i = HELL_ROWS * 10; i < npx && i + lag < TEST_COLS * HELL_ROWS; i++) {    // after the AGC settles
            if (i + lag < 0)
                continue;
            const int d = got_cols[i / HELL_ROWS][i % HELL_ROWS] > 127;
            ok += d == px[i + lag];
            n++;
        }
        if (ok > best) {
            best = ok;
            best_lag = lag;
            compared = n;
        }
    }
    const float share = compared ? (float)best / compared : 0.0f;
    ESP_LOGW("HELL", "autoteste: %d colunas, %.1f%% dos pixeis certos (desvio %d pixeis) %s", got_n, share * 100.0f,
             best_lag, share > 0.95f ? "OK" : "FALHOU");
    hell_set_tone(0.0f);
}

#endif
