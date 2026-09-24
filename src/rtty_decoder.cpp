#include "rtty_decoder.h"

#include <string.h>
#include <math.h>

#include "config.h"

// Algorithm mirrored and validated off-target in tools/rtty_sim.py (45.45 /
// 50 / 75 baud, 170 and 850 Hz shift, reversed polarity, 10 Hz mistuning,
// +1% baud error, noise down to 6 dB SNR in 300 Hz, noise only, two keyed CW
// carriers 170 Hz apart). Keep both in sync.

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;
constexpr int WIN_MAX = 200;                      // half a bit down to 30 baud

constexpr float POL_ALPHA = 1.0f / (3.0f * FS);   // polarity statistics, ~3 s
constexpr float SQ_ALPHA = 1.0f / (0.15f * FS);   // squelch average, ~150 ms
// Mean |d|: noise alone 0.27..0.33, FSK at 3 dB SNR (300 Hz) 0.43..0.53.
constexpr float SQ_OPEN = 0.42f, SQ_CLOSE = 0.38f;
constexpr float IDLE_MARK_LEVEL = 0.2f;

// Frame quality: share of recent characters with a valid stop bit. Text is
// shown, and RTTY counts as active, only above FQ_SHOW.
constexpr float FQ_ALPHA = 0.15f, FQ_START = 0.6f, FQ_SHOW = 0.5f;

// FSK keeps one tone on all the time, so the total level has no gaps; two
// keyed CW carriers 170 Hz apart (seen on the air) often have both off.
constexpr float LEVEL_ALPHA = 1.0f / (1.0f * FS);
constexpr float GAP_ALPHA = 1.0f / (0.5f * FS);
constexpr float GAP_LEVEL = 0.3f, GAP_MAX = 0.15f;

constexpr int FIGS_CODE = 0x1B, LTRS_CODE = 0x1F;
// ITA2 letters and US-TTY figures, indexed by the 5-bit code. '\0' = nothing
// to print (NUL, CR, BELL, shifts); '\n' is shown as a space.
const char LTRS[32] = {
    0, 'E', '\n', 'A', ' ', 'S', 'I', 'U', 0, 'D', 'R', 'J', 'N', 'F', 'C', 'K',
    'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q', 'O', 'B', 'G', 0, 'M', 'X', 'V', 0,
};
const char FIGS[32] = {
    0, '3', '\n', '-', ' ', 0, '8', '7', 0, '$', '4', '\'', ',', '!', ':', '(',
    '5', '"', ')', '2', '#', '6', '0', '1', '9', '?', '&', 0, '.', '/', ';', 0,
};

struct Tone {
    float cr = 1.0f, ci = 0.0f;    // oscillator phasor
    float wr = 1.0f, wi = 0.0f;    // per-sample rotation
    float ring_r[WIN_MAX], ring_i[WIN_MAX];
    float acc_r = 0.0f, acc_i = 0.0f;

    void tune(float hz)
    {
        const float w = 2.0f * (float)M_PI * hz / FS;
        wr = cosf(w);
        wi = -sinf(w);
        cr = 1.0f;
        ci = 0.0f;
    }
};

Tone tone[2];            // [0] = lower frequency, [1] = higher
float f_lo = 0.0f, f_hi = 0.0f;
float baud = RTTY_DEFAULT_BAUD;
RttyPolarity polarity = RTTY_POL_AUTO;

int win = 132;           // detector window: half a bit
int pos = 0;
float bit_len = FS / RTTY_DEFAULT_BAUD;

float hi_share = 0.5f;   // fraction of time the high tone dominates
bool hi_mark = false;
float quality = 0.0f;    // mean |d| for the squelch
float level = 0.0f;      // slow mean of the two-tone level
float gaps = 0.0f;       // share of time that level drops out
bool open = false;
float frame_quality = FQ_START;

// UART
bool framing = false;
int mark_run = 0;
float since_edge = 0.0f; // samples since the estimated start-bit edge
int nbits = 0;
int bits = 0;
bool figs = false;

char text[RTTY_TEXT_MAX + 1];
size_t text_len = 0;

void emit_char(char c)
{
    if (text_len < RTTY_TEXT_MAX)
        text[text_len++] = c;
}

void emit_code(int code)
{
    frame_quality += (1.0f - frame_quality) * FQ_ALPHA;
    if (code == LTRS_CODE) {
        figs = false;
        return;
    }
    if (code == FIGS_CODE) {
        figs = true;
        return;
    }
    const char c = (figs ? FIGS : LTRS)[code];
    if (c == ' ')
        figs = false;    // unshift on space (USOS)
    if (!c || frame_quality < FQ_SHOW)
        return;          // nothing to print, or mostly framing errors: not RTTY
    emit_char(c == '\n' ? ' ' : c);
}

void reset_state()
{
    for (Tone &t : tone) {
        memset(t.ring_r, 0, sizeof(t.ring_r));
        memset(t.ring_i, 0, sizeof(t.ring_i));
        t.acc_r = t.acc_i = 0.0f;
    }
    pos = 0;
    hi_share = 0.5f;
    hi_mark = false;
    quality = 0.0f;
    level = 0.0f;
    gaps = 0.0f;
    open = false;
    frame_quality = FQ_START;
    framing = false;
    mark_run = 0;
    figs = false;
}

void configure_timing()
{
    bit_len = FS / baud;
    win = (int)lroundf(bit_len / 2.0f);
    if (win < 8) win = 8;
    if (win > WIN_MAX) win = WIN_MAX;
}

bool hi_is_mark()
{
    if (polarity == RTTY_POL_NORMAL)
        return false;    // mark = lower tone
    if (polarity == RTTY_POL_REVERSE)
        return true;
    // RTTY idles on mark and every stop bit is mark: the tone that dominates
    // over seconds (with hysteresis) is the mark.
    if (hi_share > 0.55f)
        hi_mark = true;
    else if (hi_share < 0.45f)
        hi_mark = false;
    return hi_mark;
}

// d > 0: mark. The half-bit window centred at t - T/4 reports the line state,
// so a mark->space step at e crosses zero at e + T/4; bit k (0 = start,
// 1..5 data, 6 = stop) is read at its centre + T/4.
void uart(float d)
{
    const float T = bit_len;
    if (!framing) {
        if (d > IDLE_MARK_LEVEL) {
            mark_run++;
        } else if (d < 0.0f && mark_run > T / 2.0f) {
            framing = true;
            since_edge = T / 4.0f;
            nbits = 0;
            bits = 0;
            mark_run = 0;
        } else if (mark_run > 0) {
            mark_run--;
        }
        return;
    }

    since_edge += 1.0f;
    if (since_edge < (nbits + 0.5f) * T + T / 4.0f)
        return;
    const int bit = d > 0.0f ? 1 : 0;
    if (nbits == 0) {
        if (bit != 0)
            framing = false;    // false start
    } else if (nbits <= 5) {
        bits |= bit << (nbits - 1);    // LSB first
    } else {
        if (bit == 1)
            emit_code(bits);
        else
            frame_quality += (0.0f - frame_quality) * FQ_ALPHA;    // framing error
        framing = false;
        mark_run = (int)(T / 2.0f) + 1;    // the stop bit counts as idle mark
        return;
    }
    nbits++;
}

} // namespace

void rtty_init()
{
    f_lo = f_hi = 0.0f;
    text_len = 0;
    configure_timing();
    reset_state();
}

void rtty_set_tones(float f1, float f2)
{
    if (f1 <= 0.0f || f2 <= 0.0f) {
        if (f_lo > 0.0f) {
            f_lo = f_hi = 0.0f;
            reset_state();
        }
        return;
    }
    const float lo = fminf(f1, f2), hi = fmaxf(f1, f2);
    // Small drifts are fine inside the detector bandwidth (~2 x baud).
    if (f_lo > 0.0f && fabsf(lo - f_lo) < RTTY_RETUNE_HZ && fabsf(hi - f_hi) < RTTY_RETUNE_HZ)
        return;
    if (f_lo == 0.0f)
        reset_state();
    f_lo = lo;
    f_hi = hi;
    tone[0].tune(lo);
    tone[1].tune(hi);
}

void rtty_set_baud(float b)
{
    if (b < 30.0f || b > 300.0f || b == baud)
        return;
    baud = b;
    configure_timing();
    reset_state();
}

void rtty_set_polarity(RttyPolarity pol)
{
    polarity = pol;
}

float rtty_mark_hz()
{
    if (f_lo <= 0.0f)
        return 0.0f;
    return (polarity == RTTY_POL_REVERSE || (polarity == RTTY_POL_AUTO && hi_mark)) ? f_hi : f_lo;
}

float rtty_space_hz()
{
    if (f_lo <= 0.0f)
        return 0.0f;
    return rtty_mark_hz() == f_hi ? f_lo : f_hi;
}

float rtty_baud()
{
    return baud;
}

bool rtty_active()
{
    return f_lo > 0.0f && open && frame_quality >= FQ_SHOW;
}

void rtty_process(const float *x, int n)
{
    if (f_lo <= 0.0f)
        return;

    for (int i = 0; i < n; i++) {
        float mag[2];
        for (int k = 0; k < 2; k++) {
            Tone &t = tone[k];
            const float zr = x[i] * t.cr;
            const float zi = x[i] * t.ci;
            t.acc_r += zr - t.ring_r[pos];
            t.acc_i += zi - t.ring_i[pos];
            t.ring_r[pos] = zr;
            t.ring_i[pos] = zi;
            // advance the oscillator
            const float nr = t.cr * t.wr - t.ci * t.wi;
            const float ni = t.cr * t.wi + t.ci * t.wr;
            t.cr = nr;
            t.ci = ni;
            mag[k] = sqrtf(t.acc_r * t.acc_r + t.acc_i * t.acc_i);
        }
        if (++pos >= win) {
            pos = 0;
            // Once per window: re-sum exactly (float drift) and renormalise
            // the oscillators.
            for (Tone &t : tone) {
                float sr = 0.0f, si = 0.0f;
                for (int j = 0; j < win; j++) {
                    sr += t.ring_r[j];
                    si += t.ring_i[j];
                }
                t.acc_r = sr;
                t.acc_i = si;
                const float g = 1.0f / sqrtf(t.cr * t.cr + t.ci * t.ci);
                t.cr *= g;
                t.ci *= g;
            }
        }

        const float tot = mag[0] + mag[1];
        if (tot <= 1e-9f)
            continue;
        hi_share += ((mag[1] > mag[0] ? 1.0f : 0.0f) - hi_share) * POL_ALPHA;
        float d = (mag[1] - mag[0]) / tot;    // +1 high tone, -1 low tone

        quality += (fabsf(d) - quality) * SQ_ALPHA;
        level += (tot - level) * LEVEL_ALPHA;
        gaps += ((tot < GAP_LEVEL * level ? 1.0f : 0.0f) - gaps) * GAP_ALPHA;
        if (quality > SQ_OPEN && gaps < GAP_MAX)
            open = true;
        else if (quality < SQ_CLOSE || gaps > GAP_MAX)
            open = false;
        if (!open) {
            framing = false;    // squelched: noise, not FSK
            mark_run = 0;
            continue;
        }

        if (!hi_is_mark())
            d = -d;    // d > 0 means mark
        uart(d);
    }
}

size_t rtty_take_text(char *buf, size_t size)
{
    if (size == 0)
        return 0;
    const size_t n = text_len < size - 1 ? text_len : size - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    text_len = 0;
    return n;
}
