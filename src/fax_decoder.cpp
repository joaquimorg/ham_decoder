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
// line start.
constexpr int PULSE = W / 20;
constexpr float PULSE_CONTRAST = 0.3f;
constexpr int PHASE_AGREE = 8;                   // consecutive lines that agree...
constexpr int PHASE_TOL = W / 100;               // ...to within 1% of a line
constexpr int PHASE_MAX_LINES = 70;              // give up (and skip) after this
constexpr float PHASE_FLAT = 0.15f;              // std. deviation outside the pulse

volatile bool req_start = false, req_stop = false;
int lpm = FAX_DEFAULT_LPM, ioc = FAX_DEFAULT_IOC;
bool auto_start = true;

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
int centres[PHASE_AGREE];
int n_centres = 0;

// Tone detector
bool hi = false;
int since_edge = 0, win_n = 0, edges = 0, good_start = 0, good_stop = 0;
int start_run = 0, stop_run = 0;

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
    line_samples = FS * 60.0f / lpm * (1.0f + FAX_CLOCK_PPM * 1e-6f);
    pos = 0.0f;
    col = 0;
    acc = 0.0f;
    acc_n = 0;
    lines = image_lines = 0;
    n_centres = 0;
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

void end_of_line()
{
    lines++;
    if (state == FAX_PHASING) {
        const int c = pulse_centre();
        if (c < 0) {
            n_centres = 0;
        } else {
            if (n_centres > 0 && circ_dist(c, centres[n_centres - 1]) > PHASE_TOL)
                n_centres = 0;
            centres[n_centres++] = c;
        }
        if (n_centres == PHASE_AGREE) {
            // Delay the line start to the pulse centre: the next line begins there.
            pos -= c * line_samples / W;
            if (pos >= 0.0f)
                pos -= line_samples;
            state = FAX_RECEIVING;
        } else if (lines >= PHASE_MAX_LINES) {
            state = FAX_RECEIVING;    // no phasing seen: keep the line start as is
        }
        return;
    }
    if (lines < PHASE_MAX_LINES && looks_like_phasing())
        return;
    send_line();
    if (image_lines >= FAX_MAX_LINES)
        end_image();
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

void fax_request_start()
{
    req_start = true;
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
