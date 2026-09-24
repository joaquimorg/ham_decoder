#include "cw_decoder.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <algorithm>

#include "config.h"

// Algorithm mirrored and validated off-target in tools/cw_sim.py (12..30 WPM,
// timing jitter, 8 Hz mistuning, noise down to 10 dB SNR in 100 Hz, 10 ms
// glitches, noise only). Keep both in sync.

namespace {

constexpr int TICK = DSP_SAMPLE_RATE * CW_TICK_MS / 1000;    // samples per envelope tick
constexpr float TICKS_PER_S = 1000.0f / CW_TICK_MS;

// The envelope is integrated over the last `w` ticks, w ~ dit / 3, so slower
// CW gets a narrower detection bandwidth (20 WPM: 20 ms, ~50 Hz).
constexpr int WIN_MAX = 4;          // 20 ms cap, see CW_DEBOUNCE_MAX_TICKS
constexpr float WIN_DIV = 3.0f;

// Mark/space levels come from percentiles of the recent envelope, so a missed
// mark never leaks into the noise estimate.
constexpr int HIST = (int)(1.5f * TICKS_PER_S);
constexpr float FLOOR_P = 0.20f;    // spaces dominate CW airtime
constexpr float TOP_P = 0.95f;
constexpr int LEVEL_EVERY = 20;     // ticks between level updates when locked on
constexpr int LEVEL_EVERY_SEARCH = 4;
constexpr int WARMUP = (int)(0.5f * TICKS_PER_S);    // learn the noise first

// Dit length limits: 60 WPM .. 5 WPM (dit ms = 1200 / WPM).
constexpr float DIT_MIN_TICKS = 20.0f / CW_TICK_MS;
constexpr float DIT_MAX_TICKS = 240.0f / CW_TICK_MS;
constexpr float DIT_START_TICKS = 60.0f / CW_TICK_MS;   // 20 WPM

struct MorseCode {
    const char *code;
    char ch;
};

const MorseCode morse_table[] = {
    {".-", 'A'},    {"-...", 'B'},  {"-.-.", 'C'},  {"-..", 'D'},   {".", 'E'},
    {"..-.", 'F'},  {"--.", 'G'},   {"....", 'H'},  {"..", 'I'},    {".---", 'J'},
    {"-.-", 'K'},   {".-..", 'L'},  {"--", 'M'},    {"-.", 'N'},    {"---", 'O'},
    {".--.", 'P'},  {"--.-", 'Q'},  {".-.", 'R'},   {"...", 'S'},   {"-", 'T'},
    {"..-", 'U'},   {"...-", 'V'},  {".--", 'W'},   {"-..-", 'X'},  {"-.--", 'Y'},
    {"--..", 'Z'},
    {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'}, {"....-", '4'},
    {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
    {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-..-.", '/'}, {"-...-", '='},
    {".-.-.", '+'},  {"-....-", '-'}, {".--.-.", '@'}, {"---...", ':'}, {"-.--.", '('},
    {"-.--.-", ')'}, {".----.", '\''}, {".-..-.", '"'}, {"...-.-", '*'}, // SK -> '*'
};

// Tuning (NCO) and per-tick quadrature sums.
float tone_hz = 0.0f;
float phase = 0.0f;
float phase_inc = 0.0f;
float acc_i = 0.0f, acc_q = 0.0f;
int acc_n = 0;
float ring_i[WIN_MAX], ring_q[WIN_MAX];
int ring_pos = 0;

// Envelope history and levels.
float hist[HIST];
float sorted_hist[HIST];
int hist_len = 0, hist_pos = 0;
float floor_level = 0.0f, top_level = 0.0f;
int ticks = 0;

// Keying state (in ticks).
bool key_down = false;
int pending = 0;          // ticks the raw decision has disagreed with key_down
int run_ticks = 0;        // length of the current mark or space
bool carrier = false;     // current mark is too long to be Morse

// Dits and dahs are tracked as two clusters split at their geometric mean, so a
// dah never drags the dit estimate. A single estimate ran away on real audio
// (30 WPM drifting to 6 WPM): once it rose, 120 ms dahs counted as dits and
// pushed it further up.
float dit_ticks = DIT_START_TICKS;
float dah_ticks = 3.0f * DIT_START_TICKS;

float min_contrast = CW_MIN_CONTRAST;    // runtime-adjustable (web settings)

char symbol[8];
int symbol_len = 0;
bool symbol_overflow = false;     // more elements than any Morse character

// Per-report statistics for CW_DEBUG.
struct {
    int marks, dahs, carriers, chars, usable_ticks, ticks;
    int mark_min, mark_max, space_min;
} stats;
bool word_gap_sent = true;

char text[CW_TEXT_MAX + 1];
size_t text_len = 0;

void emit(char c)
{
    if (text_len < CW_TEXT_MAX)
        text[text_len++] = c;
}

void flush_symbol()
{
    if (symbol_len == 0 && !symbol_overflow)
        return;
    symbol[symbol_len] = 0;
    char c = '_';    // unknown sequence
    if (!symbol_overflow) {
        for (const MorseCode &m : morse_table) {
            if (strcmp(m.code, symbol) == 0) {
                c = m.ch;
                break;
            }
        }
    }
    emit(c);
    stats.chars++;
    symbol_len = 0;
    symbol_overflow = false;
    word_gap_sent = false;
}

// Share of recent samples shorter than half their estimate.
float short_share = 0.0f;

// Moves an estimate a quarter of the way to a sample (at most double it). A
// sample under half the estimate is a glitch (noise burst) while such samples
// are rare: it is ignored, or a stream of blips walks the dit estimate down
// to the 60 WPM floor (seen on the board: 30 WPM CW decoded as "T T TT").
// When they become common the estimate itself is too slow, and they count.
float learn(float est, float len)
{
    const bool is_short = len < 0.5f * est;
    short_share += ((is_short ? 1.0f : 0.0f) - short_share) * 0.1f;
    if (is_short) {
        if (short_share < CW_SHORT_SHARE)
            return est;
        len = 0.5f * est;
    }
    if (len > 2.0f * est) len = 2.0f * est;
    return est + 0.25f * (len - est);
}

// Morse dahs are ~3 dits: keep the clusters between 2x and 4x apart.
void keep_ratio()
{
    if (dit_ticks < DIT_MIN_TICKS) dit_ticks = DIT_MIN_TICKS;
    if (dit_ticks > DIT_MAX_TICKS) dit_ticks = DIT_MAX_TICKS;
    if (dah_ticks < 2.0f * dit_ticks) dah_ticks = 2.0f * dit_ticks;
    if (dah_ticks > 4.0f * dit_ticks) dah_ticks = 4.0f * dit_ticks;
}

void end_mark(int len)
{
    if (carrier) {
        carrier = false;
        symbol_len = 0;
        symbol_overflow = false;
        stats.carriers++;
        return;
    }

    const bool dah = (float)len * len >= dit_ticks * dah_ticks;
    if (symbol_len < (int)sizeof(symbol) - 1)
        symbol[symbol_len++] = dah ? '-' : '.';
    else
        symbol_overflow = true;    // decodes as unknown

    stats.marks++;
    stats.dahs += dah;
    if (stats.mark_min == 0 || len < stats.mark_min) stats.mark_min = len;
    if (len > stats.mark_max) stats.mark_max = len;

    if (len > 2.0f * dah_ticks)
        return;                    // merged elements: don't learn from it
    if (dah)
        dah_ticks = learn(dah_ticks, (float)len);
    else
        dit_ticks = learn(dit_ticks, (float)len);
    keep_ratio();
}

void end_space(int len)
{
    if (stats.space_min == 0 || len < stats.space_min)
        stats.space_min = len;
    // Gaps inside a character last one dit: the most common interval.
    if ((float)len * len < dit_ticks * dah_ticks) {
        dit_ticks = learn(dit_ticks, (float)len);
        keep_ratio();
    }
}

// Called every tick while the key is up, with the space length so far.
void space_tick(int len)
{
    if ((symbol_len > 0 || symbol_overflow) && len >= 2.0f * dit_ticks)
        flush_symbol();
    if (!word_gap_sent && len >= 5.0f * dit_ticks) {
        emit(' ');
        word_gap_sent = true;
    }
}

float percentile(int n, float p)
{
    const int k = (int)(p * (n - 1));
    std::nth_element(sorted_hist, sorted_hist + k, sorted_hist + n);
    return sorted_hist[k];
}

void update_levels()
{
    memcpy(sorted_hist, hist, hist_len * sizeof(float));
    floor_level = percentile(hist_len, FLOOR_P);
    top_level = percentile(hist_len, TOP_P);
}

void envelope_tick(float mag)
{
    ticks++;
    hist[hist_pos] = mag;
    hist_pos = (hist_pos + 1) % HIST;
    if (hist_len < HIST)
        hist_len++;

    bool usable = top_level > floor_level * min_contrast;
    const int every = usable ? LEVEL_EVERY : LEVEL_EVERY_SEARCH;
    if (ticks % every == 0 || ticks == WARMUP) {
        update_levels();
        usable = top_level > floor_level * min_contrast;
    }
    if (ticks < WARMUP)
        return;

    stats.ticks++;
    stats.usable_ticks += usable;
    const float span = top_level - floor_level;
    bool raw;
    if (!usable)
        raw = false;
    else if (key_down)
        raw = mag > floor_level + 0.35f * span;
    else
        raw = mag > floor_level + 0.5f * span;

    // Debounce: a state change must hold for a fraction of a dit, between
    // CW_DEBOUNCE_TICKS and CW_DEBOUNCE_MAX_TICKS. The cap matters: a speed
    // estimate that is too slow must not hide the real elements, or it could
    // never correct itself.
    int debounce = (int)lroundf(CW_DEBOUNCE_DIT * dit_ticks);
    if (debounce < CW_DEBOUNCE_TICKS)
        debounce = CW_DEBOUNCE_TICKS;
    if (debounce > CW_DEBOUNCE_MAX_TICKS)
        debounce = CW_DEBOUNCE_MAX_TICKS;
    if (raw != key_down) {
        if (++pending < debounce) {
            run_ticks++;
            if (!key_down)
                space_tick(run_ticks);
            return;
        }
        // The pending ticks belong to the new state.
        const int finished = run_ticks - (pending - 1);
        if (key_down)
            end_mark(finished);
        else
            end_space(finished);
        key_down = raw;
        run_ticks = pending;
        pending = 0;
    } else {
        pending = 0;
        run_ticks++;
    }

    if (key_down) {
        if (run_ticks > 10.0f * dit_ticks)
            carrier = true;    // steady tone or tuning carrier, not Morse
    } else {
        space_tick(run_ticks);
    }
}

void reset_state()
{
    acc_i = acc_q = 0.0f;
    acc_n = 0;
    memset(ring_i, 0, sizeof(ring_i));
    memset(ring_q, 0, sizeof(ring_q));
    ring_pos = 0;
    hist_len = hist_pos = 0;
    floor_level = top_level = 0.0f;
    ticks = 0;
    key_down = false;
    pending = 0;
    run_ticks = 0;
    carrier = false;
    symbol_len = 0;
    symbol_overflow = false;
    word_gap_sent = true;
    short_share = 0.0f;
}

} // namespace

void cw_init()
{
    tone_hz = 0.0f;
    text_len = 0;
    dit_ticks = DIT_START_TICKS;
    dah_ticks = 3.0f * DIT_START_TICKS;
    reset_state();
}

void cw_set_tone(float hz)
{
    if (hz <= 0.0f) {
        if (tone_hz > 0.0f) {
            flush_symbol();
            tone_hz = 0.0f;
            reset_state();
        }
        return;
    }
    // Small drifts are fine inside the detection bandwidth.
    if (tone_hz > 0.0f && fabsf(hz - tone_hz) < CW_RETUNE_HZ)
        return;
    if (tone_hz == 0.0f)
        reset_state();
    tone_hz = hz;
    phase_inc = 2.0f * (float)M_PI * hz / DSP_SAMPLE_RATE;
}

void cw_set_min_contrast(float ratio)
{
    if (ratio < 1.5f) ratio = 1.5f;
    if (ratio > 20.0f) ratio = 20.0f;
    min_contrast = ratio;
}

float cw_tone_hz()
{
    return tone_hz;
}

float cw_wpm()
{
    return 1200.0f / (dit_ticks * CW_TICK_MS);
}

void cw_process(const float *x, int n)
{
    if (tone_hz <= 0.0f)
        return;

    for (int i = 0; i < n; i++) {
        acc_i += x[i] * cosf(phase);
        acc_q += x[i] * sinf(phase);
        phase += phase_inc;
        if (phase > 2.0f * (float)M_PI)
            phase -= 2.0f * (float)M_PI;

        if (++acc_n < TICK)
            continue;

        ring_i[ring_pos] = acc_i;
        ring_q[ring_pos] = acc_q;
        ring_pos = (ring_pos + 1) % WIN_MAX;
        acc_i = acc_q = 0.0f;
        acc_n = 0;

        int w = (int)lroundf(dit_ticks / WIN_DIV);
        if (w < 2) w = 2;
        if (w > WIN_MAX) w = WIN_MAX;
        float wi = 0.0f, wq = 0.0f;
        for (int k = 1; k <= w; k++) {
            const int idx = (ring_pos - k + WIN_MAX) % WIN_MAX;
            wi += ring_i[idx];
            wq += ring_q[idx];
        }
        envelope_tick(sqrtf(wi * wi + wq * wq) / (w * TICK));
    }
}

size_t cw_take_text(char *buf, size_t size)
{
    if (size == 0)
        return 0;
    size_t n = text_len < size - 1 ? text_len : size - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    text_len = 0;
    return n;
}

size_t cw_debug_line(char *buf, size_t size)
{
    const int n = snprintf(buf, size,
        "cw: dit %.0f ms, traco %.0f ms | marcas %d (tracos %d, %d..%d ms) espaco min %d ms | portadora %d | chars %d | "
        "util %d%% | nivel %.4f/%.4f (x%.1f)",
        dit_ticks * CW_TICK_MS, dah_ticks * CW_TICK_MS, stats.marks, stats.dahs,
        stats.mark_min * CW_TICK_MS, stats.mark_max * CW_TICK_MS, stats.space_min * CW_TICK_MS,
        stats.carriers, stats.chars,
        stats.ticks ? 100 * stats.usable_ticks / stats.ticks : 0,
        floor_level, top_level, floor_level > 0 ? top_level / floor_level : 0.0f);
    memset(&stats, 0, sizeof(stats));
    return n > 0 ? (size_t)n : 0;
}
