#include "fax_decoder.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "config.h"
#include "web_ui.h"

// Algorithm mirrored and validated off-target in tools/fax_sim.py. Keep both
// in sync.

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;
constexpr int W = FAX_WIDTH;
constexpr float BLACK_HZ = 1500.0f, WHITE_HZ = 2300.0f;

// APT tones: the picture alternates black/white at the tone frequency, so the
// discriminator output crosses the middle (1900 Hz) twice per cycle.
constexpr float MID_HZ = 1900.0f, HYST_HZ = 150.0f;
constexpr int TONE_WIN = DSP_SAMPLE_RATE / 4;    // 0.25 s windows
constexpr int TONE_RUNS = 6;                     // 1.5 s of tone to trigger
constexpr float STOP_HZ = 450.0f;
constexpr float TONE_TOL = 0.3f;                 // half-period tolerance
constexpr float TONE_GOOD = 0.8f;                // share of regular half periods

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
constexpr float PH_OUTLIER = 6.0f, PH_RMS = 3.0f;    // px
constexpr float SLANT_MAX = 0.05f;               // line length within 5% of nominal
constexpr int PHASE_MAX_LINES = 80;              // give up (and skip) after this
constexpr float PHASE_FLAT = 0.15f;              // std. deviation outside the pulse

volatile bool req_start = false, req_stop = false;
volatile float req_shift = -1.0f;
int lpm = FAX_DEFAULT_LPM, ioc = FAX_DEFAULT_IOC;
bool auto_start = true;
float rate = FS;               // measured sample rate
float slant = 1.0f;            // line length correction from the phasing fit

FaxState state = FAX_IDLE;
uint32_t web_id = 0;           // web_image_* id of the current image
float line_samples = 0.0f;     // samples per line
float pos = 0.0f;              // sample position in the current line
int col = 0;
float acc = 0.0f;
int acc_n = 0;
float line[W];
int lines = 0;                 // lines since the start (phasing included)
int image_lines = 0;           // lines sent to the page
float ph_k[PH_MAX], ph_u[PH_MAX];    // line number, unwrapped pulse column
int n_ph = 0, ph_misses = 0;
int ph_last = 0;                     // last pulse column (wrapped)

// Tone detector
bool hi = false;
int since_edge = 0, win_n = 0, edges = 0, good_start = 0, good_stop = 0;
int start_run = 0, stop_run = 0;

float nominal_line()
{
    return rate * 60.0f / lpm * (1.0f + FAX_CLOCK_PPM * 1e-6f) * slant;
}

float start_tone_hz()
{
    return ioc == 288 ? 675.0f : 300.0f;
}

bool regular(int interval, float tone_hz)
{
    const float half = FS / (2.0f * tone_hz);
    return fabsf(interval - half) <= TONE_TOL * half;
}

// Window verdict: `good` regular half periods of a tone that should give
// `expected` edges per window.
bool tone_match(int good, float tone_hz)
{
    const float expected = 2.0f * tone_hz * TONE_WIN / FS;
    return good >= TONE_GOOD * expected && edges <= (1.0f + TONE_TOL) * expected;
}

void begin_image(bool phasing)
{
    char title[32];
    snprintf(title, sizeof(title), "FAX %d lpm IOC %d", lpm, ioc);
    web_id = web_image_begin(title, W, 1, W / ((float)M_PI * ioc));
    slant = 1.0f;
    line_samples = nominal_line();
    pos = 0.0f;
    col = 0;
    acc = 0.0f;
    acc_n = 0;
    lines = image_lines = 0;
    n_ph = ph_misses = 0;
    state = phasing ? FAX_PHASING : FAX_RECEIVING;
}

void end_image()
{
    if (state != FAX_IDLE)
        web_image_end(web_id);
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

void send_line()
{
    uint8_t px[W];
    for (int i = 0; i < W; i++) {
        const float v = line[i] * 255.0f + 0.5f;
        px[i] = v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t)v;
    }
    web_image_line(web_id, px);
    image_lines++;
}

// Least-squares line u = a + s*k through the phasing pulses, twice dropping
// points further than PH_OUTLIER from it. False when it does not fit.
bool phase_fit(float &a, float &s)
{
    bool use[PH_MAX];
    for (int i = 0; i < n_ph; i++)
        use[i] = true;
    for (int iter = 0; iter < 3; iter++) {
        double n = 0, sk = 0, su = 0, skk = 0, sku = 0;
        for (int i = 0; i < n_ph; i++) {
            if (!use[i])
                continue;
            n++;
            sk += ph_k[i];
            su += ph_u[i];
            skk += (double)ph_k[i] * ph_k[i];
            sku += (double)ph_k[i] * ph_u[i];
        }
        const double den = n * skk - sk * sk;
        if (n < PH_MIN || den <= 0.0)
            return false;
        s = (float)((n * sku - sk * su) / den);
        a = (float)((su - s * sk) / n);
        double r2 = 0;
        for (int i = 0; i < n_ph; i++) {
            const float r = ph_u[i] - (a + s * ph_k[i]);
            if (use[i])
                r2 += r * r;
            if (iter < 2)
                use[i] = fabsf(r) <= PH_OUTLIER;
        }
        if (iter == 2)
            return sqrt(r2 / n) <= PH_RMS && fabsf(s) <= SLANT_MAX * W;
    }
    return false;
}

// Phasing line ended: collect its pulse; once phasing is over, correct the
// line length and move the line start onto the pulse.
void phasing_line()
{
    const int c = pulse_centre();
    float u = (float)c;
    if (c >= 0 && n_ph) {
        int d = c - ph_last;
        if (d > W / 2)
            d -= W;
        else if (d < -W / 2)
            d += W;
        u = ph_u[n_ph - 1] + d;
    }
    // Once the drift is known, a "pulse" off the line is image content (the
    // phasing is over), not a phasing pulse.
    float a, s;
    if (c >= 0 && n_ph >= PH_MIN && phase_fit(a, s) && fabsf(u - (a + s * lines)) > 2.0f * PH_OUTLIER)
        u = -1e9f;
    if (c >= 0 && u > -1e8f) {
        ph_last = c;
        if (n_ph < PH_MAX) {
            ph_k[n_ph] = (float)lines;
            ph_u[n_ph] = u;
            n_ph++;
        }
        ph_misses = 0;
    } else {
        ph_misses++;
    }
    const bool over = (n_ph >= PH_MIN && ph_misses >= PH_END_MISSES) || n_ph >= PH_MAX ||
                      lines >= PHASE_MAX_LINES;
    if (!over)
        return;
    if (phase_fit(a, s)) {
        // Pulse column in the next line, at the line length used so far.
        float p = fmodf(a + s * (lines + 1), (float)W);
        if (p < 0.0f)
            p += W;
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
    since_edge++;
    const bool edge = hi ? hz < MID_HZ - HYST_HZ : hz > MID_HZ + HYST_HZ;
    if (edge) {
        hi = !hi;
        edges++;
        if (regular(since_edge, start_tone_hz()))
            good_start++;
        if (regular(since_edge, STOP_HZ))
            good_stop++;
        since_edge = 0;
    }
    if (++win_n < TONE_WIN)
        return;
    start_run = tone_match(good_start, start_tone_hz()) ? start_run + 1 : 0;
    stop_run = tone_match(good_stop, STOP_HZ) ? stop_run + 1 : 0;
    win_n = edges = good_start = good_stop = 0;

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

void fax_process(const float *hz, int n)
{
    const float shift = req_shift;
    if (shift > 0.0f) {
        req_shift = -1.0f;
        if (state != FAX_IDLE) {
            // Lines already sent are rotated on the page; the next ones start
            // that much later (the partial line in progress is lost).
            const int col = (int)(shift * W + 0.5f);
            web_id = web_image_rotate(web_id, col);
            pos -= col * line_samples / W;
            if (pos >= 0.0f)
                pos -= line_samples;
            col_reset();
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
        if (state != FAX_IDLE) {
            const float v = (hz[i] - BLACK_HZ) / (WHITE_HZ - BLACK_HZ);
            add_sample(v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v);
        }
    }
}
