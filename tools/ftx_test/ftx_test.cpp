// PC test for src/ftx_core.cpp: synthetic FT8/FT4 slots (GFSK as in
// ft8_lib's demo/gen_ft8.c) with noise, fed in DSP-sized blocks from an
// arbitrary UTC time, as the firmware does. Build and run: tools/ftx_test/run.sh
#define _USE_MATH_DEFINES
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <random>
#include <string>
#include <vector>

#include "ftx_core.h"
#include <ft8/constants.h>
#include <ft8/encode.h>
#include <ft8/message.h>

static const int FS = 12000;
static const int BLOCK = 1024;    // FFT_SIZE: what analyzer_process_block() gets

// GFSK synthesis from ft8_lib demo/gen_ft8.c (MIT).
static void gfsk_pulse(int n_spsym, float bt, float *pulse)
{
    const float k = 5.336446f;    // pi * sqrt(2 / ln 2)
    for (int i = 0; i < 3 * n_spsym; ++i) {
        const float t = i / (float)n_spsym - 1.5f;
        pulse[i] = (erff(k * bt * (t + 0.5f)) - erff(k * bt * (t - 0.5f))) / 2;
    }
}

static std::vector<float> synth_gfsk(const uint8_t *sym, int n_sym, float f0, float bt, float period)
{
    const int n_spsym = (int)(0.5f + FS * period), n_wave = n_sym * n_spsym;
    const float dphi_peak = 2 * (float)M_PI / n_spsym;
    std::vector<float> dphi(n_wave + 2 * n_spsym, 2 * (float)M_PI * f0 / FS), pulse(3 * n_spsym), out(n_wave);
    gfsk_pulse(n_spsym, bt, pulse.data());
    for (int i = 0; i < n_sym; ++i)
        for (int j = 0; j < 3 * n_spsym; ++j)
            dphi[j + i * n_spsym] += dphi_peak * sym[i] * pulse[j];
    for (int j = 0; j < 2 * n_spsym; ++j) {
        dphi[j] += dphi_peak * pulse[j + n_spsym] * sym[0];
        dphi[j + n_sym * n_spsym] += dphi_peak * pulse[j] * sym[n_sym - 1];
    }
    float phi = 0;
    for (int k = 0; k < n_wave; ++k) {
        out[k] = sinf(phi);
        phi = fmodf(phi + dphi[k + n_spsym], 2 * (float)M_PI);
    }
    const int n_ramp = n_spsym / 8;
    for (int i = 0; i < n_ramp; ++i) {
        const float env = (1 - cosf(2 * (float)M_PI * i / (2 * n_ramp))) / 2;
        out[i] *= env;
        out[n_wave - 1 - i] *= env;
    }
    return out;
}

struct Tx {
    int slot;           // slot number from the start of the recording's first whole slot
    const char *text;
    float freq, snr_db, dt;
};

struct Rx {
    FtxMessage m;
};

static std::vector<Rx> got;
static void on_msg(const FtxMessage &m, void *)
{
    got.push_back({m});
}

// Returns the number of failures.
static int run(const char *name, FtxProtocol proto, const std::vector<Tx> &txs, int n_slots,
               double t_start, float noise_rms, unsigned seed)
{
    const bool ft4 = proto == FTX_FT4;
    const double period = ft4 ? 7.5 : 15.0;
    const float sym_period = ft4 ? 0.048f : 0.16f, bt = ft4 ? 1.0f : 2.0f;
    const double first_slot = ceil(t_start / period) * period;    // first whole slot
    const int n = (int)((first_slot - t_start + n_slots * period + 1.0) * FS);
    std::vector<float> x(n, 0.0f);

    std::mt19937 rng(seed);
    std::normal_distribution<float> gauss(0.0f, noise_rms);
    for (float &v : x)
        v = gauss(rng);
    // Noise power in 2500 Hz: noise_rms^2 * 2500 / (FS / 2).
    const float pn = noise_rms * noise_rms * 2500.0f / (FS / 2.0f);
    for (const Tx &t : txs) {
        ftx_message_t msg;
        if (ftx_message_encode(&msg, nullptr, t.text) != FTX_MESSAGE_RC_OK) {
            printf("  nao codifica '%s'\n", t.text);
            return 1;
        }
        uint8_t tones[FT4_NN];
        if (ft4)
            ft4_encode(msg.payload, tones);
        else
            ft8_encode(msg.payload, tones);
        std::vector<float> s = synth_gfsk(tones, ft4 ? FT4_NN : FT8_NN, t.freq, bt, sym_period);
        const float amp = sqrtf(2.0f * pn * powf(10.0f, t.snr_db / 10.0f));
        const int at = (int)((first_slot + t.slot * period + 0.5 + t.dt - t_start) * FS);
        for (size_t i = 0; i < s.size() && at + (int)i < n; i++)
            x[at + i] += amp * s[i];
    }

    got.clear();
    ftx_core_set_protocol(proto);
    for (int i = 0; i + BLOCK <= n; i += BLOCK) {
        double slot = 0;
        const int h = ftx_core_feed(&x[i], BLOCK, t_start + (double)i / FS, &slot);
        if (h >= 0) {
            ftx_core_decode(h, slot, on_msg, nullptr);
            ftx_core_release(h);
        }
    }

    printf("%s\n", name);
    int fails = 0;
    for (const Tx &t : txs) {
        const double slot = first_slot + t.slot * period;
        const Rx *r = nullptr;
        for (const Rx &g : got)
            if (fabs(g.m.slot_start - slot) < 0.01 && !strcmp(g.m.text, t.text))
                r = &g;
        if (!r) {
            printf("  FALTA  %-24s %6.0f Hz  snr %5.1f  dt %+.2f\n", t.text, t.freq, t.snr_db, t.dt);
            fails++;
            continue;
        }
        const bool ok = fabsf(r->m.freq_hz - t.freq) < (ft4 ? 12.0f : 4.0f) &&
                        fabsf(r->m.dt - t.dt) < (ft4 ? 0.06f : 0.1f) && fabsf(r->m.snr_db - t.snr_db) < 2.0f;
        printf("  %s %-24s %6.1f Hz (%6.0f)  snr %5.1f (%5.1f)  dt %+.2f (%+.2f)\n", ok ? "ok    " : "ERRADO",
               r->m.text, r->m.freq_hz, t.freq, r->m.snr_db, t.snr_db, r->m.dt, t.dt);
        fails += !ok;
    }
    for (const Rx &g : got) {
        bool expected = false;
        for (const Tx &t : txs)
            expected |= !strcmp(g.m.text, t.text) && fabs(g.m.slot_start - (first_slot + t.slot * period)) < 0.01;
        if (!expected) {
            printf("  A MAIS %s\n", g.m.text);
            fails++;
        }
    }
    return fails;
}

int main()
{
    if (!ftx_core_init(FS)) {
        printf("ftx_core_init falhou\n");
        return 1;
    }
    const double t0 = 1790000007.3;    // mid-slot: the first partial slot is skipped
    int fails = 0;
    fails += run("FT8: 4 sinais em 2 slots", FTX_FT8,
                 { { 0, "CQ EA1ABC IN52", 600.0f, -8.0f, 0.0f },
                   { 0, "CT1XYZ EA1ABC -12", 1203.0f, -15.0f, 0.3f },
                   { 0, "EA1ABC CT1XYZ R-10", 2107.0f, -18.0f, -0.4f },
                   { 1, "CQ DX CT7ABC IM58", 1500.0f, -12.0f, 1.2f } },
                 3, t0, 0.05f, 1);
    fails += run("FT8: sinal forte", FTX_FT8, { { 0, "CQ EA1ABC IN52", 900.0f, 5.0f, 0.1f } },
                 1, 1790000003.9, 0.02f, 2);
    fails += run("FT4: 3 sinais", FTX_FT4,
                 { { 0, "CQ EA1ABC IN52", 800.0f, -8.0f, 0.0f },
                   { 0, "CT1XYZ EA1ABC -05", 1500.0f, -12.0f, 0.2f },
                   { 1, "EA1ABC CT1XYZ RR73", 2200.0f, -10.0f, -0.1f } },
                 3, 1790000002.0, 0.05f, 3);
    fails += run("FT8: so ruido", FTX_FT8, {}, 4, t0, 0.1f, 4);
    printf("slots perdidos: %d\n%s\n", ftx_core_skipped(), fails ? "FALHOU" : "OK");
    return fails ? 1 : 0;
}
