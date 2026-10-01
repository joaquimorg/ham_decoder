#include "fax_decoder.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "ui_hub.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

// Algorithm mirrored and validated off-target in tools/fax_sim.py. Keep both
// in sync.

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;
constexpr int W = FAX_WIDTH;
constexpr float BLACK_HZ = 1500.0f, WHITE_HZ = 2300.0f;

// APT tones: the discriminator output is modulated at the tone frequency, but
// not always as a full black/white swing (a real stop tone was a 1900 Hz
// carrier with sidebands at +-450 Hz: a small oscillation off 1900 Hz). So the
// tone is found by the share of the output's variance at that frequency
// (Goertzel over 0.25 s): a pure tone ~0.3..0.8, image content and noise < 0.02.
constexpr float MID_HZ = 1900.0f;
constexpr int TONE_WIN = DSP_SAMPLE_RATE / 4;    // 0.25 s windows
constexpr int TONE_RUNS = 6;                     // 1.5 s of tone to trigger
constexpr float STOP_HZ = 450.0f;
constexpr float TONE_SHARE = 0.1f;

// Phasing lines: black with a white pulse 5% of the line long, centred on the
// line start. With the transmitter's line rate slightly off ours (sample clock
// error: easily 0.1..0.5% with the internal ADC) the pulse drifts from line to
// line: a straight-line fit of its position gives both the true line length
// (slant) and the line start.
constexpr int PULSE = W / 20;
constexpr float PULSE_CONTRAST = 0.3f;
constexpr int PHASE_TOL = W / 100;               // leftover phasing line: pulse within 1%
constexpr int PH_MIN = 12, PH_MAX = 64;          // pulses to fit
constexpr int PH_END_MISSES = 1;                 // lines without pulse: phasing over
constexpr int PH_EDGE_EXCUSE = 2;                // lines at the edge not counted as misses
constexpr int PH_SEG_MIN = 3;                    // points for a segment's own intercept
constexpr float PH_OUTLIER = 6.0f, PH_RMS = 3.0f;    // px
// The measured sample rate already corrects the board clock and stations send
// at an exact rate: what is left is tiny. A larger fitted drift is a wrong fit
// (real phasing differing from the model, image content), and is rejected.
constexpr float SLANT_MAX = 0.003f;              // line length within 0.3% of the measured rate
constexpr int PHASE_MAX_LINES = 80;              // give up (and skip) after this
constexpr float PHASE_FLAT = 0.15f;              // std. deviation outside the pulse

volatile bool req_start = false, req_stop = false;
volatile float req_shift = -1.0f;
int lpm = FAX_DEFAULT_LPM, ioc = FAX_DEFAULT_IOC;
bool auto_start = true;
float rate = FS;               // measured sample rate
float slant = 1.0f;            // line length correction from the phasing fit

FaxState state = FAX_IDLE;
uint32_t img_id = 0;           // ui_image_* id of the current image
float line_samples = 0.0f;     // samples per line
float pos = 0.0f;              // sample position in the current line
int col = 0;
float acc = 0.0f;
int acc_n = 0;
float line[W];
int lines = 0;                 // lines since the start (phasing included)
int image_lines = 0;           // lines sent to the page
float ph_k[PH_MAX], ph_u[PH_MAX];    // line number, pulse column
// Pulse across the line edge: its halves come from consecutive transmitted
// lines, already apart by the drift, which biases its centre (and the slope).
bool ph_edge[PH_MAX];
int ph_seg[PH_MAX];                  // segment: +1 each time the pulse crosses the edge
int n_ph = 0, ph_misses = 0, ph_excused = 0;
int ph_last = 0;                     // last pulse column (wrapped)

// Tone detector: Goertzel filters at the start and stop tones, plus the
// output's sum and sum of squares, over each window.
struct Goertzel {
    float coeff = 0.0f, s1 = 0.0f, s2 = 0.0f;
    void set(float f) { coeff = 2.0f * cosf(2.0f * (float)M_PI * f / FS); }
    void step(float x)
    {
        const float s = x + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    float power() const { return s1 * s1 + s2 * s2 - coeff * s1 * s2; }
    void clear() { s1 = s2 = 0.0f; }
};
Goertzel g_start, g_stop;
float tone_for = 0.0f;           // start tone the filter is set for
double sum = 0.0, sum2 = 0.0;
int win_n = 0, start_run = 0, stop_run = 0;

// Image path: weak signal -> white (squelch), then a median of 5 against impulses.
float sq_env = -1.0f, sq_smooth = 0.0f;
float med_hist[5];
bool med_primed = false;

// Line length tracking.
// Internal RAM is tight (Wi-Fi, web server): these live in PSRAM.
EXT_RAM_BSS_ATTR float trk_prev[W];
EXT_RAM_BSS_ATTR float trk_cur[W];
bool trk_has_prev = false;
float trk_lags[FAX_TRK_LAGS];
int trk_n = 0;

float nominal_line()
{
    return rate * 60.0f / lpm * (1.0f + FAX_CLOCK_PPM * 1e-6f) * slant;
}

float start_tone_hz()
{
    return ioc == 288 ? 675.0f : 300.0f;
}

// Share of the window's variance at the Goertzel filter's frequency (a pure
// sine gives ~1).
float tone_share(const Goertzel &g)
{
    const double n = TONE_WIN, var = sum2 / n - (sum / n) * (sum / n);
    return var > 0.0 ? (float)(2.0 * g.power() / (n * n * var)) : 0.0f;
}

void begin_image(bool phasing)
{
    char title[32];
    snprintf(title, sizeof(title), "FAX %d lpm IOC %d", lpm, ioc);
    img_id = ui_image_begin(title, W, 1, W / ((float)M_PI * ioc));
    slant = 1.0f;
    line_samples = nominal_line();
    pos = 0.0f;
    col = 0;
    acc = 0.0f;
    acc_n = 0;
    lines = image_lines = 0;
    n_ph = ph_misses = ph_excused = 0;
    trk_has_prev = false;
    trk_n = 0;
    state = phasing ? FAX_PHASING : FAX_RECEIVING;
}

void end_image()
{
    if (state != FAX_IDLE)
        ui_image_end(img_id);
    state = FAX_IDLE;
}

// Centre column of the phasing pulse in `line`, or -1 when the line has no
// clear pulse. The pulse is the window that differs most from the line mean.
int pulse_centre()
{
    float total = 0.0f;
    for (int i = 0; i < W; i++)
        total += line[i];
    const float mean = total / W;
    float win = 0.0f;
    for (int i = 0; i < PULSE; i++)
        win += line[i];
    float best = 0.0f;
    int best_at = 0;
    for (int s = 0; s < W; s++) {
        const float d = fabsf(win / PULSE - mean);
        if (d > best) {
            best = d;
            best_at = s;
        }
        win += line[(s + PULSE) % W] - line[s];
    }
    return best >= PULSE_CONTRAST ? (best_at + PULSE / 2) % W : -1;
}

int circ_dist(int a, int b)
{
    const int d = abs(a - b) % W;
    return d < W - d ? d : W - d;
}

// A leftover phasing line once aligned: pulse around column 0, flat elsewhere.
bool looks_like_phasing()
{
    const int c = pulse_centre();
    if (c < 0 || circ_dist(c, 0) > PHASE_TOL)
        return false;
    float sum = 0.0f, sq = 0.0f;
    const int n = W - 2 * PULSE;
    for (int i = PULSE; i < W - PULSE; i++) {
        sum += line[i];
        sq += line[i] * line[i];
    }
    const float mean = sum / n;
    return sq / n - mean * mean < PHASE_FLAT * PHASE_FLAT;
}

// Line length and jumps of the signal, from the content of the line against the
// last line that matched it (a noisy line is not used as the reference, up to
// FAX_REF_AGE lines). A shift of FAX_JUMP_MIN px or more at once is the signal
// jumping (the audio lost or gained samples): the line start moves with it. A
// small one is the line length being off: every FAX_TRK_LAGS lines the median
// corrects it. Mirrored in fax_sim.py (track_line).
void track_line()
{
    constexpr int L = FAX_JUMP_MAX_LAG;
    static float prev_norm = 0.0f;
    static int ref_age = 1;
    const float flat = FAX_TRK_MIN_STD * sqrtf((float)W);
    float mean = 0.0f;
    for (int i = 0; i < W; i++)
        mean += line[i];
    mean /= W;
    float norm = 0.0f;
    for (int i = 0; i < W; i++) {
        trk_cur[i] = line[i] - mean;
        norm += trk_cur[i] * trk_cur[i];
    }
    norm = sqrtf(norm);
    if (norm < flat) {
        ref_age++;
        return;
    }
    bool good = false;
    if (trk_has_prev && ref_age <= FAX_REF_AGE) {
        float c[2 * L + 3];    // correlation at lags -(L+1) .. L+1
        for (int k = -L - 1; k <= L + 1; k++) {
            float d = 0.0f;
            for (int i = 0; i < W; i++)
                d += trk_prev[i] * trk_cur[(i + k + W) % W];
            c[k + L + 1] = d / (norm * prev_norm);
        }
        int k = 1;
        for (int j = 2; j <= 2 * L + 1; j++)
            if (c[j] > c[k])
                k = j;
        // Repeating content (text, stripes) has several peaks: take the nearest one.
        for (int j = 1; j <= 2 * L + 1; j++)
            if (c[j] >= 0.9f * c[k] && c[j] >= c[j - 1] && c[j] >= c[j + 1] && abs(j - (L + 1)) < abs(k - (L + 1)))
                k = j;
        const float den = c[k - 1] - 2.0f * c[k] + c[k + 1];
        float frac = den < 0.0f ? 0.5f * (c[k - 1] - c[k + 1]) / den : 0.0f;
        frac = frac < -0.5f ? -0.5f : frac > 0.5f ? 0.5f : frac;
        const float lag = (float)(k - (L + 1)) + frac;
        good = c[k] >= FAX_REF_CORR;
        if (fabsf(lag) >= FAX_JUMP_MIN && c[k] >= FAX_JUMP_MIN_CORR && c[k] - c[L + 1] >= FAX_JUMP_MARGIN) {
            pos -= lag * line_samples / W;
            trk_n = 0;
            trk_has_prev = false;    // the next line is not comparable with this one
            return;
        }
        if (good && ref_age == 1 && fabsf(lag) <= FAX_TRK_MAX_LAG && trk_n < FAX_TRK_LAGS)
            trk_lags[trk_n++] = lag;
    }
    if (good || !trk_has_prev || ref_age >= FAX_REF_AGE) {
        memcpy(trk_prev, trk_cur, sizeof(trk_prev));
        prev_norm = norm;
        trk_has_prev = true;
        ref_age = 1;
    } else {
        ref_age++;
    }
    if (trk_n < FAX_TRK_LAGS)
        return;
    for (int i = 1; i < trk_n; i++) {    // insertion sort, then the median
        const float v = trk_lags[i];
        int j = i - 1;
        for (; j >= 0 && trk_lags[j] > v; j--)
            trk_lags[j + 1] = trk_lags[j];
        trk_lags[j + 1] = v;
    }
    const float d = trk_n % 2 ? trk_lags[trk_n / 2] : 0.5f * (trk_lags[trk_n / 2 - 1] + trk_lags[trk_n / 2]);
    trk_n = 0;
    if (fabsf(d) < FAX_TRK_DEADBAND)
        return;
    slant *= 1.0f + FAX_TRK_GAIN * d / W;
    slant = slant < 1.0f - FAX_TRK_LIMIT ? 1.0f - FAX_TRK_LIMIT : slant > 1.0f + FAX_TRK_LIMIT ? 1.0f + FAX_TRK_LIMIT : slant;
    line_samples = nominal_line();
}

void send_line()
{
    uint8_t px[W];
    for (int i = 0; i < W; i++) {
        const float v = line[i] * 255.0f + 0.5f;
        px[i] = v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t)v;
    }
    ui_image_line(img_id, px);
    image_lines++;
}

// Fit of the phasing pulses: column u = a_g + s*k, one slope s (the drift per
// line: slant) and one intercept per segment g. A new segment starts each time
// the pulse crosses the line edge: from then on the pulse seen in a line is the
// next transmitted one, one drift step away from a continuous line. Pulses
// across the edge (biased centre) are left out when enough others remain;
// points further than PH_OUTLIER are dropped twice. Returns the intercept of
// the last segment (the one the next line belongs to). False when it does not fit.
bool phase_fit(float &a_last, float &s)
{
    bool base[PH_MAX], use[PH_MAX];
    int inner = 0;
    for (int i = 0; i < n_ph; i++)
        inner += !ph_edge[i];
    for (int i = 0; i < n_ph; i++)
        base[i] = use[i] = inner >= PH_MIN ? !ph_edge[i] : true;
    const int n_seg = n_ph ? ph_seg[n_ph - 1] + 1 : 0;
    static double gn[PH_MAX], gk[PH_MAX], gu[PH_MAX];    // per segment (off the small stack)
    for (int iter = 0; iter < 3; iter++) {
        for (int g = 0; g < n_seg; g++)
            gn[g] = gk[g] = gu[g] = 0.0;
        int n = 0;
        for (int i = 0; i < n_ph; i++) {
            if (!use[i])
                continue;
            const int g = ph_seg[i];
            gn[g]++;
            gk[g] += ph_k[i];
            gu[g] += ph_u[i];
            n++;
        }
        if (n < PH_MIN)
            return false;
        for (int g = 0; g < n_seg; g++)
            if (gn[g] > 0.0) {
                gk[g] /= gn[g];
                gu[g] /= gn[g];
            }
        double sxy = 0.0, sxx = 0.0;
        for (int i = 0; i < n_ph; i++) {
            if (!use[i])
                continue;
            const int g = ph_seg[i];
            sxy += (ph_k[i] - gk[g]) * (ph_u[i] - gu[g]);
            sxx += (ph_k[i] - gk[g]) * (ph_k[i] - gk[g]);
        }
        if (sxx <= 0.0)
            return false;
        s = (float)(sxy / sxx);
        double r2 = 0.0;
        for (int i = 0; i < n_ph; i++) {
            const int g = ph_seg[i];
            if (gn[g] <= 0.0) {
                use[i] = false;
                continue;
            }
            const float r = ph_u[i] - (float)(gu[g] + s * (ph_k[i] - gk[g]));
            if (use[i])
                r2 += r * r;
            if (iter < 2)
                use[i] = base[i] && fabsf(r) <= PH_OUTLIER;
        }
        if (iter == 2) {
            // Intercept of the last segment with enough points, carried over
            // the later edge crossings: +(W+s) each (drift left), -(W+s) (drift right).
            int g = n_seg - 1;
            while (g > 0 && gn[g] < PH_SEG_MIN)
                g--;
            if (gn[g] <= 0.0)
                return false;
            a_last = (float)(gu[g] - s * gk[g]);
            for (int later = g + 1; later < n_seg; later++)
                a_last += s < 0.0f ? W + s : -(W + s);
            return sqrt(r2 / n) <= PH_RMS && fabsf(s) <= SLANT_MAX * W;
        }
    }
    return false;
}

// Phasing line ended: collect its pulse; once phasing is over, correct the
// line length and move the line start onto the pulse.
void phasing_line()
{
    const int c = pulse_centre();
    const bool wrapped = c >= 0 && n_ph && abs(c - ph_last) > W / 2;
    // Once the drift is known, a "pulse" off the line is image content (the
    // phasing is over), not a phasing pulse. Across the edge the pulse also
    // moves one drift step.
    float a, s;
    bool ok = c >= 0, at_edge = false;
    if (n_ph >= PH_MIN && phase_fit(a, s)) {
        // Where the fit expects this line's pulse: near or across the edge the
        // pulse is split, doubled (drift left) or missing (drift right), and
        // its absence is not the end of phasing.
        const float raw = a + s * lines;
        float pred = fmodf(raw, (float)W);
        if (pred < 0.0f)
            pred += W;
        at_edge = raw < PULSE / 2 + PH_OUTLIER || raw > W - PULSE / 2 - PH_OUTLIER;
        if (ok)
            ok = circ_dist(c, (int)lroundf(pred) % W) <= PH_OUTLIER + (wrapped || at_edge ? fabsf(s) : 0.0f);
    }
    if (ok) {
        if (n_ph < PH_MAX) {
            ph_k[n_ph] = (float)lines;
            ph_u[n_ph] = (float)c;
            ph_seg[n_ph] = n_ph ? ph_seg[n_ph - 1] + (wrapped ? 1 : 0) : 0;
            ph_edge[n_ph] = circ_dist(c, 0) <= PULSE / 2 + 2;
            n_ph++;
        }
        ph_last = c;
        ph_misses = ph_excused = 0;
    } else if (at_edge && ph_excused < PH_EDGE_EXCUSE) {
        ph_excused++;
    } else {
        ph_misses++;
    }
    const bool over = (n_ph >= PH_MIN && ph_misses >= PH_END_MISSES) || n_ph >= PH_MAX ||
                      lines >= PHASE_MAX_LINES;
    if (!over)
        return;
    if (phase_fit(a, s)) {
        // Pulse column in the next line, at the line length used so far. Past
        // the left edge the pulse in that line is the next transmitted one,
        // W + s further (s < 0); past the right edge the line has none and the
        // start lies beyond it (s > 0), which the delay below allows.
        float p = a + s * (lines + 1);
        while (p < 0.0f)
            p += W + (s < 0.0f ? s : 0.0f);
        pos -= p * line_samples / W;
        slant *= 1.0f + s / W;
        line_samples = nominal_line();
        if (pos >= 0.0f)
            pos -= line_samples;
    }
    state = FAX_RECEIVING;    // without a fit: keep the line start as is
}

void end_of_line()
{
    lines++;
    if (state == FAX_PHASING) {
        phasing_line();
        return;
    }
    line_samples = nominal_line();
    if (lines < PHASE_MAX_LINES && looks_like_phasing())
        return;
    track_line();
    send_line();
    if (image_lines >= FAX_MAX_LINES)
        end_image();
}

// Restarts the pixel accumulation (after moving `pos`).
void col_reset()
{
    col = 0;
    acc = 0.0f;
    acc_n = 0;
}

void put_pixel()
{
    if (acc_n)
        line[col] = acc / acc_n;
    acc = 0.0f;
    acc_n = 0;
}

void add_sample(float v)
{
    if (pos >= 0.0f) {
        int c = (int)(pos * W / line_samples);
        if (c >= W)
            c = W - 1;
        if (c != col) {
            put_pixel();
            for (int i = col + 1; i < c; i++)
                line[i] = line[col];
            col = c;
        }
        acc += v;
        acc_n++;
    }
    pos += 1.0f;
    if (pos >= line_samples) {
        put_pixel();
        pos -= line_samples;
        col = 0;
        end_of_line();
    }
}

void tone_sample(float hz)
{
    if (tone_for != start_tone_hz()) {
        tone_for = start_tone_hz();
        g_start.set(tone_for);
        g_stop.set(STOP_HZ);
    }
    const float x = hz - MID_HZ;
    g_start.step(x);
    g_stop.step(x);
    sum += x;
    sum2 += (double)x * x;
    if (++win_n < TONE_WIN)
        return;
    start_run = tone_share(g_start) >= TONE_SHARE ? start_run + 1 : 0;
    stop_run = tone_share(g_stop) >= TONE_SHARE ? stop_run + 1 : 0;
    g_start.clear();
    g_stop.clear();
    sum = sum2 = 0.0;
    win_n = 0;

    if (stop_run == TONE_RUNS)
        end_image();
    else if (start_run == TONE_RUNS && auto_start && state != FAX_PHASING) {
        end_image();
        begin_image(true);
    }
}

} // namespace

void fax_init()
{
    state = FAX_IDLE;
}

void fax_set_lpm(int v)
{
    if (v == 60 || v == 90 || v == 120 || v == 240)
        lpm = v;
}

void fax_set_ioc(int v)
{
    if (v == 576 || v == 288)
        ioc = v;
}

void fax_set_auto(bool on)
{
    auto_start = on;
}

void fax_set_sample_rate(float hz)
{
    if (hz > 0.95f * FS && hz < 1.05f * FS)
        rate = hz;
}

void fax_request_start()
{
    req_start = true;
}

void fax_request_shift(float share)
{
    if (share > 0.0f && share < 1.0f)
        req_shift = share;
}

void fax_request_stop()
{
    req_stop = true;
}

FaxState fax_state()
{
    return state;
}

int fax_lines()
{
    return image_lines;
}

// Image value 0..1 for one sample: weak carrier -> white, then a median of 5
// (the output lags 2 samples; the tone detector sees the raw frequency).
float image_value(float f, float m)
{
    constexpr float A_ENV = 1.0f / (FAX_SQ_ENV_TC * FS), A_SM = 1.0f / (FAX_SQ_SMOOTH * FS);
    if (sq_env < 0.0f)
        sq_env = sq_smooth = m;
    sq_env += (m - sq_env) * A_ENV;
    sq_smooth += (m - sq_smooth) * A_SM;
    if (sq_smooth < FAX_SQ_LEVEL * sq_env)
        f = WHITE_HZ;
    if (!med_primed) {
        for (float &h : med_hist)
            h = f;
        med_primed = true;
    }
    for (int i = 0; i < 4; i++)
        med_hist[i] = med_hist[i + 1];
    med_hist[4] = f;
    float t[5];
    for (int i = 0; i < 5; i++) {
        float v = med_hist[i];
        int j = i - 1;
        for (; j >= 0 && t[j] > v; j--)
            t[j + 1] = t[j];
        t[j + 1] = v;
    }
    const float v = (t[2] - BLACK_HZ) / (WHITE_HZ - BLACK_HZ);
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

void fax_process(const float *hz, int n, const float *mag)
{
    const float shift = req_shift;
    if (shift > 0.0f) {
        req_shift = -1.0f;
        if (state != FAX_IDLE) {
            // Lines already sent are rotated on the page; the next ones start
            // that much later (the partial line in progress is lost).
            const int col = (int)(shift * W + 0.5f);
            img_id = ui_image_rotate(img_id, col);
            pos -= col * line_samples / W;
            if (pos >= 0.0f)
                pos -= line_samples;
            col_reset();
            trk_has_prev = false;
        }
    }
    if (req_stop) {
        req_stop = false;
        end_image();
    }
    if (req_start) {
        req_start = false;
        end_image();
        begin_image(false);
    }
    for (int i = 0; i < n; i++) {
        tone_sample(hz[i]);
        // Always run, so the filters are settled when an image starts.
        const float v = image_value(hz[i], mag ? mag[i] : 1.0f);
        if (state != FAX_IDLE)
            add_sample(v);
    }
}
