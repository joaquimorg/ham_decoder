#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>

#include "analyzer.h"
#include "config.h"
#include "cw_decoder.h"
#include "rtty_decoder.h"
#include "fm_demod.h"
#include "fax_decoder.h"
#include "sstv_decoder.h"
#include "ftx_decoder.h"
#include "capture.h"
#include "classifier.h"
#include "settings.h"
#include "web_ui.h"

namespace {

constexpr int N = FFT_SIZE;
constexpr int HALF = N / 2;
constexpr float BIN_HZ = (float)DSP_SAMPLE_RATE / N;

// Hann window: a full-scale sine gives |X| = N/4, so this maps it to 0 dBFS.
constexpr float POWER_NORM = 16.0f / ((float)N * (float)N);

constexpr int ENV_BLOCK = DSP_SAMPLE_RATE / 100; // 10 ms envelope blocks
constexpr int ENV_MAX = 256;

constexpr int MAX_PEAKS = 4;
constexpr int MAX_CANDIDATES = 64;

struct Peak {
    float hz;
    float db;
};

float hann[N];
float tw_cos[HALF];
float tw_sin[HALF];
float re[N];
float im[N];
float psd[HALF];
float db[HALF];
float scratch[HALF];
float psd_avg[HALF];

int frames = 0;
int unlock_reports = 0;
int rtty_unlock_reports = 0;
int32_t peak_raw = 0;
uint32_t last_overruns = 0;
uint32_t report_no = 0;

double level_acc = 0;
uint32_t level_n = 0;

float env_db[ENV_MAX];
int env_count = 0;
double env_acc = 0;
int env_n = 0;

void fft(float *xr, float *xi)
{
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = xr[i]; xr[i] = xr[j]; xr[j] = t;
            t = xi[i]; xi[i] = xi[j]; xi[j] = t;
        }
    }

    for (int len = 2; len <= N; len <<= 1) {
        const int h = len / 2;
        const int step = N / len;
        for (int i = 0; i < N; i += len) {
            for (int k = 0; k < h; k++) {
                const float wr = tw_cos[k * step];
                const float wi = -tw_sin[k * step];
                const int a = i + k;
                const int b = a + h;
                const float tr = xr[b] * wr - xi[b] * wi;
                const float ti = xr[b] * wi + xi[b] * wr;
                xr[b] = xr[a] - tr;
                xi[b] = xi[a] - ti;
                xr[a] += tr;
                xi[a] += ti;
            }
        }
    }
}

int cmp_float(const void *a, const void *b)
{
    const float fa = *(const float *)a;
    const float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

int cmp_peak_desc(const void *a, const void *b)
{
    const float da = ((const Peak *)a)->db;
    const float db_ = ((const Peak *)b)->db;
    return (da < db_) - (da > db_);
}

float percentile(const float *v, int n, float p)
{
    memcpy(scratch, v, n * sizeof(float));
    qsort(scratch, n, sizeof(float), cmp_float);
    int idx = (int)(p * (n - 1) + 0.5f);
    return scratch[idx];
}

// Nearest standard RTTY/FSK shift, or 0.
int standard_shift(float shift)
{
    static const int shifts[] = {170, 200, 425, 450, 850};
    for (int s : shifts) {
        if (fabsf(shift - s) <= s * 0.12f)
            return s;
    }
    return 0;
}

void print_ruler()
{
    char line[WATERFALL_COLS + 1];
    memset(line, ' ', WATERFALL_COLS);
    line[WATERFALL_COLS] = 0;

    const int cols_per_500 = 500 / WATERFALL_HZ_PER_COL;
    for (int c = 0, hz = 0; c < WATERFALL_COLS; c += cols_per_500, hz += 500) {
        char lbl[8];
        int len = snprintf(lbl, sizeof(lbl), "%d", hz);
        for (int i = 0; i < len && c + i < WATERFALL_COLS; i++)
            line[c + i] = lbl[i];
    }
    printf("\n      |%s| Hz\n", line);
}

void report()
{
    const float inv = 1.0f / frames;

    for (int k = 0; k < HALF; k++) {
        psd_avg[k] = psd[k] * inv;
        db[k] = 10.0f * log10f(psd_avg[k] + 1e-14f);
    }

    // TinyML classifier (runs alongside the heuristic one below).
    const MlResult ml = classifier_run(psd_avg, env_db, env_count);
#if ML_LOG_FEATURES
    {
        int nf;
        const float *f = classifier_features(&nf);
        printf("ML_F,%s", ML_LOG_LABEL);
        for (int i = 0; i < nf; i++)
            printf(",%.4f", f[i]);
        printf("\n");
    }
#endif

    int kmin = (int)ceilf(SPECTRUM_MIN_HZ / BIN_HZ);
    int kmax = (int)floorf(SPECTRUM_MAX_HZ / BIN_HZ);
    if (kmax > HALF - 2)
        kmax = HALF - 2;
    const int nrange = kmax - kmin + 1;

    // Noise floor = median bin level inside the audio passband.
    const float noise_db = percentile(&db[kmin], nrange, 0.5f);

    // Local maxima above the floor, parabolic-interpolated.
    Peak cand[MAX_CANDIDATES];
    int ncand = 0;
    for (int k = kmin + 1; k < kmax && ncand < MAX_CANDIDATES; k++) {
        if (db[k] < noise_db + PEAK_MIN_DB)
            continue;
        if (!(db[k] >= db[k - 1] && db[k] > db[k + 1]))
            continue;
        const float a = db[k - 1], b = db[k], c = db[k + 1];
        const float den = a - 2.0f * b + c;
        const float p = den != 0.0f ? 0.5f * (a - c) / den : 0.0f;
        cand[ncand].hz = (k + p) * BIN_HZ;
        cand[ncand].db = b - 0.25f * (a - c) * p;
        ncand++;
    }
    qsort(cand, ncand, sizeof(Peak), cmp_peak_desc);

    Peak peaks[MAX_PEAKS];
    int npeaks = 0;
    for (int i = 0; i < ncand && npeaks < MAX_PEAKS; i++) {
        bool close = false;
        for (int j = 0; j < npeaks; j++) {
            if (fabsf(cand[i].hz - peaks[j].hz) < PEAK_MIN_SEP_HZ)
                close = true;
        }
        if (!close)
            peaks[npeaks++] = cand[i];
    }

    // Occupied bandwidth and how concentrated the energy is around the top two peaks.
    int occ_bins = 0;
    int occ_first = -1, occ_last = -1;
    double sig_total = 0, sig_near = 0;
    for (int k = kmin; k <= kmax; k++) {
        if (db[k] < noise_db + 6.0f)
            continue;
        const double lin = psd[k] * inv;
        sig_total += lin;
        const float hz = k * BIN_HZ;
        for (int j = 0; j < npeaks && j < 2; j++) {
            if (fabsf(hz - peaks[j].hz) <= 50.0f) {
                sig_near += lin;
                break;
            }
        }
        if (db[k] >= noise_db + PEAK_MIN_DB) {
            occ_bins++;
            if (occ_first < 0)
                occ_first = k;
            occ_last = k;
        }
    }
    const float concentration = sig_total > 0 ? (float)(sig_near / sig_total) : 0.0f;
    const float occ_hz = occ_bins * BIN_HZ;

    // On/off keying from the 10 ms wideband envelope.
    int edges = 0;
    float env_range = 0;
    if (env_count >= 10) {
        const float p10 = percentile(env_db, env_count, 0.1f);
        const float p90 = percentile(env_db, env_count, 0.9f);
        env_range = p90 - p10;
        if (env_range >= KEYING_MIN_DB) {
            const float hi = p10 + 0.6f * env_range;
            const float lo = p10 + 0.4f * env_range;
            bool on = env_db[0] > hi;
            for (int i = 1; i < env_count; i++) {
                if (!on && env_db[i] > hi) {
                    on = true;
                    edges++;
                } else if (on && env_db[i] < lo) {
                    on = false;
                }
            }
        }
    }
    const float seconds = env_count / 100.0f;
    const float edges_per_s = seconds > 0 ? edges / seconds : 0;

    // Heuristic classification.
    char label[48];
    const float snr = npeaks > 0 ? peaks[0].db - noise_db : 0.0f;
    bool narrow_tone = false;
    float fsk_lo = 0.0f, fsk_hi = 0.0f;

    if (npeaks == 0 || snr < SIGNAL_MIN_SNR_DB) {
        snprintf(label, sizeof(label), "RUIDO");
    } else if (npeaks >= 2 && concentration > 0.7f &&
               fabsf(peaks[0].db - peaks[1].db) < 10.0f &&
               fabsf(peaks[0].hz - peaks[1].hz) >= 100.0f &&
               fabsf(peaks[0].hz - peaks[1].hz) <= 1000.0f &&
               env_range < KEYING_MIN_DB) {
        const float f_lo = fminf(peaks[0].hz, peaks[1].hz);
        const float f_hi = fmaxf(peaks[0].hz, peaks[1].hz);
        const float shift = f_hi - f_lo;
        const int std = standard_shift(shift);
        fsk_lo = f_lo;
        fsk_hi = f_hi;
        if (rtty_active())
            snprintf(label, sizeof(label), "RTTY %.0f/%.0fHz %.0fbd", f_lo, f_hi, rtty_baud());
        else
            snprintf(label, sizeof(label), "FSK %.0f/%.0fHz sh%.0f%s",
                     f_lo, f_hi, shift, std ? " RTTY?" : "");
    } else if (concentration > 0.7f && occ_hz < 150.0f) {
        narrow_tone = true;
        if (edges_per_s >= 1.0f && edges_per_s <= 40.0f)
            snprintf(label, sizeof(label), "CW %.0fHz %.0fwpm", peaks[0].hz, cw_wpm());
        else
            snprintf(label, sizeof(label), "TOM %.0fHz", peaks[0].hz);
    } else if (snr >= SIGNAL_MIN_SNR_DB + CW_KEYED_EXTRA_SNR_DB &&
               edges_per_s >= 1.0f && edges_per_s <= 40.0f) {
        // Strong keyed tone whose harmonics or key clicks widen the spectrum:
        // still CW, decoded on the strongest peak (the fundamental).
        narrow_tone = true;
        snprintf(label, sizeof(label), "CW %.0fHz %.0fwpm", peaks[0].hz, cw_wpm());
    } else if (occ_hz >= 600.0f) {
        snprintf(label, sizeof(label), "LARGO %.0f-%.0fHz (voz?)",
                 occ_first * BIN_HZ, occ_last * BIN_HZ);
    } else {
        snprintf(label, sizeof(label), "? %.0fHz bw%.0f", peaks[0].hz, occ_hz);
    }

    // RTTY that decodes well-formed characters has priority: its mark tone
    // also looks like keyed CW to the classifier, which then fed the CW
    // decoder with RTTY and printed garbage.
    const bool rtty_on = rtty_active();
    if (rtty_on) {
        narrow_tone = false;
        snprintf(label, sizeof(label), "RTTY %.0f/%.0fHz %.0fbd",
                 rtty_mark_hz(), rtty_space_hz(), rtty_baud());
    }

    // Keep the CW decoder on the tone; hold the lock through short pauses.
    // A manual tone (web settings) overrides the automatic choice.
    if (rtty_on) {
        cw_set_tone(0.0f);
        unlock_reports = 0;
    } else if (!g_settings.cw_auto_tone) {
        cw_set_tone(g_settings.cw_tone_hz);
        unlock_reports = 0;
    } else if (narrow_tone) {
        cw_set_tone(peaks[0].hz);
        unlock_reports = 0;
    } else if (cw_tone_hz() > 0.0f && ++unlock_reports >= CW_UNLOCK_REPORTS) {
        cw_set_tone(0.0f);
    }
    char cw_text[CW_TEXT_MAX + 1];
    cw_take_text(cw_text, sizeof(cw_text));
    web_push_text(WEB_TEXT_CW, cw_text);

    // RTTY follows the two FSK tones, held through short pauses.
    rtty_set_baud(g_settings.rtty_baud);
    rtty_set_polarity((RttyPolarity)g_settings.rtty_polarity);
    if (fsk_lo > 0.0f) {
        rtty_set_tones(fsk_lo, fsk_hi);
        rtty_unlock_reports = 0;
    } else if (rtty_on) {
        rtty_unlock_reports = 0;    // still decoding: the classifier missed FSK this second
    } else if (rtty_mark_hz() > 0.0f && ++rtty_unlock_reports >= RTTY_UNLOCK_REPORTS) {
        rtty_set_tones(0.0f, 0.0f);
    }
    char rtty_text[RTTY_TEXT_MAX + 1];
    rtty_take_text(rtty_text, sizeof(rtty_text));
    web_push_text(WEB_TEXT_RTTY, rtty_text);
#if SERIAL_REPORT >= 2
    for (char *p = rtty_text; *p; p++)
        if (*p == '\n')
            *p = ' ';    // the report is one line per second
#endif

#if SERIAL_REPORT >= 2
    // Text waterfall: one character per WATERFALL_HZ_PER_COL, 4 dB per step above floor.
    static const char shades[] = " .:-=+*#%@";
    char wf[WATERFALL_COLS + 1];
    for (int c = 0; c < WATERFALL_COLS; c++) {
        int k0 = (int)(c * WATERFALL_HZ_PER_COL / BIN_HZ);
        int k1 = (int)((c + 1) * WATERFALL_HZ_PER_COL / BIN_HZ);
        if (k1 <= k0)
            k1 = k0 + 1;
        if (k1 > HALF)
            k1 = HALF;
        float m = -200.0f;
        for (int k = k0; k < k1; k++)
            m = fmaxf(m, db[k]);
        int lvl = (int)((m - noise_db) / 4.0f);
        if (lvl < 0) lvl = 0;
        if (lvl > 9) lvl = 9;
        wf[c] = shades[lvl];
    }
    wf[WATERFALL_COLS] = 0;
#endif

    const float rms_dbfs = level_n ? 10.0f * log10f((float)(level_acc / level_n) + 1e-14f) + 3.01f : -140.0f;
    const float pk_dbfs = 20.0f * log10f((float)peak_raw / AUDIO_FULL_SCALE + 1e-9f);

    WebStatus ws = {};
    strlcpy(ws.label, label, sizeof(ws.label));
    ws.snr_db = snr;
    ws.rms_dbfs = rms_dbfs;
    ws.peak_dbfs = pk_dbfs;
    ws.tone_hz = cw_tone_hz();
    ws.wpm = cw_wpm();
    ws.clip = pk_dbfs > -0.5f;
    ws.rtty_mark_hz = rtty_mark_hz();
    ws.rtty_space_hz = rtty_space_hz();
    ws.rtty_active = rtty_active();
    strlcpy(ws.ml_label, ml_class_name(ml.cls), sizeof(ws.ml_label));
    ws.ml_prob = ml.prob;
    web_push_status(ws);

#if SERIAL_REPORT >= 2
    if (report_no % WATERFALL_RULER_EVERY == 0)
        print_ruler();

    printf("%5" PRIu32 " |%s| %-28s ML %-5s %3.0f%% SNR%3.0f rms%4.0f pk%4.0f%s%s%s%s%s\n",
           report_no, wf, label, ml_class_name(ml.cls), ml.prob * 100.0f, snr, rms_dbfs, pk_dbfs,
           pk_dbfs > -0.5f ? " CLIP!" : "",
           last_overruns ? " OVR" : "",
           cw_text[0] || rtty_text[0] ? "  \"" : "", cw_text[0] ? cw_text : rtty_text,
           cw_text[0] || rtty_text[0] ? "\"" : "");

#if CW_DEBUG
    if (cw_tone_hz() > 0.0f) {
        char dbg[200];
        cw_debug_line(dbg, sizeof(dbg));
        printf("      %s\n", dbg);
    }
#endif

#if DIAG_VERBOSE
    printf("      floor %.1f dBFS | occ %.0f Hz | conc %.2f | env %.1f dB, %d edges |",
           noise_db, occ_hz, concentration, env_range, edges);
    for (int i = 0; i < npeaks; i++)
        printf(" %.1fHz/%.1f", peaks[i].hz, peaks[i].db);
    printf("\n");
#endif

#elif SERIAL_REPORT == 1
    // Compact: a line when the kind of signal changes, then the CW text as it arrives.
    static char last_kind[8] = "";
    char kind[8];
    const size_t klen = strcspn(label, " ");
    strlcpy(kind, label, klen + 1 < sizeof(kind) ? klen + 1 : sizeof(kind));
    static int last_ml = -1;
    if (strcmp(kind, last_kind) != 0 || ml.cls != last_ml) {
        printf("\n[%s  ML %s %.0f%%  SNR %.0f dB  pk %.0f dBFS]\n", label,
               ml_class_name(ml.cls), ml.prob * 100.0f, snr, pk_dbfs);
        strlcpy(last_kind, kind, sizeof(last_kind));
        last_ml = ml.cls;
    }
    if (cw_text[0] || rtty_text[0]) {
        fputs(cw_text, stdout);
        fputs(rtty_text, stdout);
        fflush(stdout);
    }
#endif

    report_no++;
}

void reset()
{
    memset(psd, 0, sizeof(psd));
    frames = 0;
    peak_raw = 0;
    level_acc = 0;
    level_n = 0;
    env_count = 0;
}

} // namespace

void analyzer_init()
{
    for (int i = 0; i < N; i++)
        hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / N);
    for (int k = 0; k < HALF; k++) {
        tw_cos[k] = cosf(2.0f * (float)M_PI * k / N);
        tw_sin[k] = sinf(2.0f * (float)M_PI * k / N);
    }
    cw_init();
    rtty_init();
    fm_demod_init();
    fax_init();
    sstv_init();
    ftx_init();
    capture_init();
    reset();
}

void analyzer_process_block(const float *x, int32_t raw_peak, uint32_t overruns)
{
    capture_push(x, N, cw_tone_hz() > 0.0f || rtty_mark_hz() > 0.0f);
    cw_process(x, N);
    rtty_process(x, N);
    static float fm_hz[N];
    fm_demod_process(x, fm_hz, N);
    fax_process(fm_hz, N);
    sstv_process(fm_hz, N);
    ftx_process(x, N);

    for (int i = 0; i < N; i++) {
        const float s = x[i];
        re[i] = s * hann[i];
        im[i] = 0.0f;

        const float sq = s * s;
        level_acc += sq;
        level_n++;

        env_acc += sq;
        if (++env_n == ENV_BLOCK) {
            if (env_count < ENV_MAX)
                env_db[env_count++] = 10.0f * log10f((float)(env_acc / ENV_BLOCK) + 1e-14f);
            env_acc = 0;
            env_n = 0;
        }
    }

    if (raw_peak > peak_raw)
        peak_raw = raw_peak;
    last_overruns = overruns;

    fft(re, im);

    // One web spectrum row per frame: 0.5 dB steps from -120 dBFS.
    static_assert(WEB_BINS <= HALF, "WEB_BINS exceeds the FFT");
    uint8_t row[WEB_BINS];
    for (int k = 0; k < HALF; k++) {
        const float p = (re[k] * re[k] + im[k] * im[k]) * POWER_NORM;
        psd[k] += p;
        if (k < WEB_BINS) {
            const float q = (10.0f * log10f(p + 1e-14f) + 120.0f) * 2.0f;
            row[k] = q <= 0.0f ? 0 : q >= 255.0f ? 255 : (uint8_t)q;
        }
    }
    web_push_spectrum(row);

    if (++frames >= FFT_FRAMES_PER_REPORT) {
        report();
        reset();
    }
}
