#include "sstv_decoder.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

#include "config.h"
#include "web_ui.h"

// Algorithm mirrored and validated off-target in tools/sstv_sim.py. Keep both
// in sync.

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;
constexpr float MS = FS / 1000.0f;                  // samples per millisecond
constexpr float BLACK_HZ = 1500.0f, WHITE_HZ = 2300.0f;

// Frequency history (Hz, int16) for decoding a line once it has all arrived:
// the longest line (Scottie DX, 1.05 s) plus the sync search window.
constexpr int RING = 16384;
constexpr int MASK = RING - 1;

// VIS header in 1 ms bins: leader 1900 Hz 300 ms, break 1200 Hz 10 ms,
// leader 300 ms, start bit 1200 Hz 30 ms, 7 data bits LSB first + even
// parity (1100 Hz = 1, 1300 Hz = 0) 30 ms each, stop bit 1200 Hz 30 ms.
constexpr int BIN = DSP_SAMPLE_RATE / 1000;
constexpr int VIS_HIST = 1024;                      // bins kept (power of 2)
constexpr int VIS_BITS_END = 300;                   // start bit .. stop bit
constexpr float VIS_TOL_HZ = 70.0f;
constexpr int VIS_RUN_MAX = 40;                     // decide at the latest after this many

// Line sync: 1200 Hz. Searched around where the timing fit expects it.
constexpr float SYNC_MAX_HZ = 1350.0f;              // mean over the pulse
constexpr float WIN_SHARE = 0.08f, WIN_MAX_MS = 30.0f;
constexpr int FIT_MIN = 8;                          // syncs before fitting the line rate
constexpr double SLANT_MAX = 0.005;                 // line rate within 0.5% of nominal
constexpr int MISS_MAX = 30;                        // lines without sync: signal gone
constexpr int GALLERY_MIN_ROWS = 32;                // shorter images are not kept

enum Kind : uint8_t { RGB, R36, R72, PD };

// Times in ms from the line start. RGB: ch = G, B, R starts. Robot 36: ch[0]
// Y, ch[1] chroma (R-Y or B-Y, told by the separator at sep_ms). Robot 72:
// Y, R-Y, B-Y. PD: Y (even row), R-Y, B-Y, Y (odd row); each line is 2 rows.
struct Mode {
    const char *name;
    uint8_t vis;
    Kind kind;
    uint16_t width, lines;      // lines as transmitted (PD: pairs of rows)
    float line_ms, sync_ms, sync_len_ms, scan_ms, scan2_ms, sep_ms;
    float ch[4];
    float first_ms;             // line 0 start after the VIS stop bit
};

#define PD_MODE(n, v, w, l, s) \
    { n, v, PD, w, l, 22.08f + 4 * (s), 0.0f, 20.0f, s, s, 0.0f, \
      { 22.08f, 22.08f + (s), 22.08f + 2 * (s), 22.08f + 3 * (s) }, 0.0f }

const Mode MODES[] = {
    { "Martin M1", 44, RGB, 320, 256, 446.446f, 0.0f, 4.862f, 146.432f, 0, 0,
      { 5.434f, 152.438f, 299.442f, 0 }, 0.0f },
    { "Martin M2", 40, RGB, 320, 256, 226.798f, 0.0f, 4.862f, 73.216f, 0, 0,
      { 5.434f, 79.222f, 153.010f, 0 }, 0.0f },
    { "Scottie S1", 60, RGB, 320, 256, 428.22f, 279.48f, 9.0f, 138.24f, 0, 0,
      { 1.5f, 141.24f, 289.98f, 0 }, 9.0f },
    { "Scottie S2", 56, RGB, 320, 256, 277.692f, 180.628f, 9.0f, 88.064f, 0, 0,
      { 1.5f, 91.064f, 191.128f, 0 }, 9.0f },
    { "Scottie DX", 76, RGB, 320, 256, 1050.3f, 695.7f, 9.0f, 345.6f, 0, 0,
      { 1.5f, 348.6f, 706.2f, 0 }, 9.0f },
    { "Robot 36", 8, R36, 320, 240, 150.0f, 0.0f, 9.0f, 88.0f, 44.0f, 100.0f,
      { 12.0f, 106.0f, 0, 0 }, 0.0f },
    { "Robot 72", 12, R72, 320, 240, 300.0f, 0.0f, 9.0f, 138.0f, 69.0f, 0,
      { 12.0f, 156.0f, 231.0f, 0 }, 0.0f },
    PD_MODE("PD50", 93, 320, 128, 91.52f),
    PD_MODE("PD90", 99, 320, 128, 170.24f),
    PD_MODE("PD120", 95, 640, 248, 121.6f),
    PD_MODE("PD160", 98, 512, 200, 195.584f),
    PD_MODE("PD180", 96, 640, 248, 183.04f),
    PD_MODE("PD240", 97, 640, 248, 244.48f),
};
constexpr int MAX_W = 640;

volatile bool req_stop = false;

int16_t *ring = nullptr;
uint64_t count = 0;             // samples ever received

// VIS detector
float bins[VIS_HIST];
uint64_t nbins = 0;
float bin_acc = 0.0f;
int bin_n = 0;
int run = 0, run_code = -1;
uint64_t run_first = 0;

// Current image
const Mode *mode = nullptr;
const Mode *last_mode = nullptr;
bool receiving = false;
uint32_t web_id = 0;
int line_no = 0, rows_sent = 0, missed = 0;
double t0 = 0.0;                // VIS end (sample), origin of the fit
double a = 0.0, b = 0.0;        // line k starts at t0 + a + b*k
double b_nom = 0.0;
double sk, st, skk, skt;        // least-squares sums over the syncs found
int n_fit = 0;

uint8_t ch_buf[4][MAX_W];
uint8_t ry_last[MAX_W], by_last[MAX_W];
uint8_t rgb[MAX_W * 3];

inline float at(uint64_t i)
{
    return ring[(uint32_t)i & MASK];
}

float seg_mean(uint64_t end_bin, int from_end, int len)
{
    // Mean of bins [end_bin - from_end, end_bin - from_end + len).
    float s = 0.0f;
    const uint64_t first = end_bin - from_end;
    for (int i = 0; i < len; i++)
        s += bins[(uint32_t)(first + i) & (VIS_HIST - 1)];
    return s / len;
}

// VIS code ending at bin e (the last bin of the stop bit), or -1. Each 30 ms
// element is judged on its central 20 ms, so the match holds for a few bins
// around the true alignment.
int vis_at(uint64_t e)
{
    if (e < 600)
        return -1;
    auto element = [&](int k) { return seg_mean(e, VIS_BITS_END - 1 - 30 * k - 5, 20); };
    if (fabsf(element(0) - 1200.0f) > VIS_TOL_HZ || fabsf(element(9) - 1200.0f) > VIS_TOL_HZ)
        return -1;
    if (fabsf(seg_mean(e, VIS_BITS_END + 250, 200) - 1900.0f) > VIS_TOL_HZ)
        return -1;
    int code = 0, ones = 0;
    for (int k = 0; k < 8; k++) {
        const float f = element(1 + k);
        const bool one = fabsf(f - 1100.0f) <= VIS_TOL_HZ;
        if (!one && fabsf(f - 1300.0f) > VIS_TOL_HZ)
            return -1;
        if (one) {
            ones++;
            if (k < 7)
                code |= 1 << k;
        }
    }
    return ones % 2 == 0 ? code : -1;
}

const Mode *mode_for(int code)
{
    for (const Mode &m : MODES)
        if (m.vis == code)
            return &m;
    return nullptr;
}

void end_image()
{
    if (receiving && rows_sent >= GALLERY_MIN_ROWS)
        web_image_archive(web_id);
    if (receiving)
        web_image_end(web_id);
    receiving = false;
}

void begin_image(const Mode *m, double vis_end)
{
    end_image();
    mode = last_mode = m;
    char title[32];
    snprintf(title, sizeof(title), "SSTV %s", m->name);
    web_id = web_image_begin(title, m->width, 3, 1.0f);
    receiving = true;
    line_no = rows_sent = missed = 0;
    t0 = vis_end;
    b_nom = b = m->line_ms * MS;
    a = m->first_ms * MS;
    sk = st = skk = skt = 0.0;
    n_fit = 0;
    for (int i = 0; i < MAX_W; i++)
        ry_last[i] = by_last[i] = 128;
}

inline uint8_t level(float hz)
{
    const float v = (hz - BLACK_HZ) * (255.0f / (WHITE_HZ - BLACK_HZ)) + 0.5f;
    return v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t)v;
}

// Mean frequency over [from, to) (absolute sample positions).
float mean_hz(double from, double to)
{
    const uint64_t i = (uint64_t)ceil(from), end = (uint64_t)ceil(to);
    if (end <= i)
        return at((uint64_t)llround(from));
    float s = 0.0f;
    for (uint64_t k = i; k < end; k++)
        s += at(k);
    return s / (end - i);
}

// One colour component: `scan_ms` long, from `start_ms` into the line.
void read_channel(double line_start, float start_ms, float scan_ms, uint8_t *out)
{
    const double r = b / b_nom * MS;    // samples per ms, slant included
    const int w = mode->width;
    for (int j = 0; j < w; j++) {
        const double from = line_start + r * (start_ms + (double)scan_ms * j / w);
        const double to = line_start + r * (start_ms + (double)scan_ms * (j + 1) / w);
        out[j] = level(mean_hz(from, to));
    }
}

void send_yuv(const uint8_t *y, const uint8_t *ry, const uint8_t *by)
{
    for (int j = 0; j < mode->width; j++) {
        const float Y = y[j], R = ry[j] - 128.0f, B = by[j] - 128.0f;
        const float c[3] = { Y + 1.402f * R, Y - 0.344f * B - 0.714f * R, Y + 1.772f * B };
        for (int k = 0; k < 3; k++)
            rgb[j * 3 + k] = c[k] <= 0.0f ? 0 : c[k] >= 255.0f ? 255 : (uint8_t)(c[k] + 0.5f);
    }
    web_image_line(web_id, rgb);
    rows_sent++;
}

// Looks for the sync of line k around the prediction and updates the fit.
void track_sync(int k)
{
    const double pred = t0 + a + b * k;
    const double win = fmin(WIN_SHARE * mode->line_ms, WIN_MAX_MS) * MS;
    const int len = (int)(mode->sync_len_ms * MS);
    const uint64_t s0 = (uint64_t)llround(pred + mode->sync_ms * MS - win);
    const int span = (int)(2 * win);
    float sum = 0.0f;
    for (int i = 0; i < len; i++)
        sum += at(s0 + i);
    float best = sum;
    int best_at = 0;
    for (int s = 1; s <= span; s++) {
        sum += at(s0 + s + len - 1) - at(s0 + s - 1);
        if (sum < best) {
            best = sum;
            best_at = s;
        }
    }
    const double t = (double)(s0 + best_at) - mode->sync_ms * MS - t0;    // measured line start
    if (best / len > SYNC_MAX_HZ || (n_fit >= FIT_MIN && fabs(t - (a + b * k)) > 0.5 * win)) {
        missed++;
        return;
    }
    missed = 0;
    n_fit++;
    sk += k;
    st += t;
    skk += (double)k * k;
    skt += k * t;
    const double den = n_fit * skk - sk * sk;
    if (n_fit >= FIT_MIN && den > 0.0) {
        b = (n_fit * skt - sk * st) / den;
        b = fmin(fmax(b, b_nom * (1.0 - SLANT_MAX)), b_nom * (1.0 + SLANT_MAX));
    }
    a = (st - b * sk) / n_fit;
}

void decode_line()
{
    const Mode &m = *mode;
    track_sync(line_no);
    const double ls = t0 + a + b * line_no;
    switch (m.kind) {
    case RGB:
        for (int c = 0; c < 3; c++)
            read_channel(ls, m.ch[c], m.scan_ms, ch_buf[c]);
        for (int j = 0; j < m.width; j++) {
            rgb[j * 3] = ch_buf[2][j];        // sent G, B, R
            rgb[j * 3 + 1] = ch_buf[0][j];
            rgb[j * 3 + 2] = ch_buf[1][j];
        }
        web_image_line(web_id, rgb);
        rows_sent++;
        break;
    case R36: {
        read_channel(ls, m.ch[0], m.scan_ms, ch_buf[0]);
        const double r = b / b_nom * MS;
        const bool is_ry = mean_hz(ls + r * m.sep_ms, ls + r * (m.sep_ms + 4.5f)) < 1900.0f;
        read_channel(ls, m.ch[1], m.scan2_ms, is_ry ? ry_last : by_last);
        send_yuv(ch_buf[0], ry_last, by_last);
        break;
    }
    case R72:
        read_channel(ls, m.ch[0], m.scan_ms, ch_buf[0]);
        read_channel(ls, m.ch[1], m.scan2_ms, ch_buf[1]);
        read_channel(ls, m.ch[2], m.scan2_ms, ch_buf[2]);
        send_yuv(ch_buf[0], ch_buf[1], ch_buf[2]);
        break;
    case PD:
        for (int c = 0; c < 4; c++)
            read_channel(ls, m.ch[c], m.scan_ms, ch_buf[c]);
        send_yuv(ch_buf[0], ch_buf[1], ch_buf[2]);
        send_yuv(ch_buf[3], ch_buf[1], ch_buf[2]);
        break;
    }
    line_no++;
    if (line_no >= m.lines || missed >= MISS_MAX)
        end_image();
}

// Whole line k (and the sync search window after it) received?
bool line_ready()
{
    const double win = fmin(WIN_SHARE * mode->line_ms, WIN_MAX_MS) * MS;
    return (double)count > t0 + a + b * line_no + b + win + 2;
}

void vis_bin(float mean)
{
    bins[(uint32_t)nbins & (VIS_HIST - 1)] = mean;
    const int code = vis_at(nbins);
    if (code >= 0 && (code == run_code || run == 0)) {
        if (run == 0)
            run_first = nbins;
        run_code = code;
        run++;
    }
    if (run > 0 && (code != run_code || run >= VIS_RUN_MAX)) {
        // The middle of the matching run is the true alignment.
        const uint64_t end_bin = run_first + (run - 1) / 2;
        const Mode *m = mode_for(run_code);
        run = 0;
        run_code = -1;
        if (m)
            begin_image(m, (double)(end_bin + 1) * BIN);
    }
    nbins++;
}

} // namespace

void sstv_init()
{
    ring = (int16_t *)heap_caps_malloc(RING * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!ring)
        ring = (int16_t *)malloc(RING * sizeof(int16_t));
}

void sstv_request_stop()
{
    req_stop = true;
}

bool sstv_receiving()
{
    return receiving;
}

const char *sstv_mode_name()
{
    return last_mode ? last_mode->name : "";
}

int sstv_lines()
{
    return rows_sent;
}

void sstv_process(const float *hz, int n)
{
    if (!ring)
        return;
    if (req_stop) {
        req_stop = false;
        end_image();
    }
    for (int i = 0; i < n; i++) {
        const float f = hz[i] < 0.0f ? 0.0f : hz[i] > 4000.0f ? 4000.0f : hz[i];
        ring[(uint32_t)count & MASK] = (int16_t)f;
        count++;
        bin_acc += f;
        if (++bin_n == BIN) {
            vis_bin(bin_acc / BIN);
            bin_acc = 0.0f;
            bin_n = 0;
        }
        while (receiving && line_ready())
            decode_line();
    }
}
