#include "psk_decoder.h"

// The project builds with -Og; this runs on every sample (with the other
// decoders it took the analysis over 100% of its time).
#pragma GCC optimize("O2")

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;
constexpr int NSPEEDS = 5;
const float SPEED_BAUD[NSPEEDS] = { 31.25f, 62.5f, 125.0f, 250.0f, 500.0f };
const char *const BPSK_NAME[NSPEEDS] = { "PSK31", "PSK63", "PSK125", "PSK250", "PSK500" };
const char *const QPSK_NAME[NSPEEDS] = { "QPSK31", "QPSK63", "QPSK125", "QPSK250", "QPSK500" };
static_assert(DSP_SAMPLE_RATE % 500 == 0, "symbol lengths must be whole samples");

// Symbol timing: the matched-filter energy is averaged in nbins phases of the
// symbol; the strongest phase is where a whole symbol fills the filter.
constexpr int NBINS_MAX = 16;
constexpr float BIN_ALPHA = 0.05f;

// Per-symbol averages.
constexpr float Q_ALPHA = 0.05f;      // phase quality
constexpr float REV_ALPHA = 0.05f;    // share of phase reversals (BPSK) / quarter steps (QPSK)
constexpr float FERR_ALPHA = 0.1f;    // frequency error
constexpr float VIT_ALPHA = 0.05f;    // Viterbi path fit
// Squelch on the quality: noise alone gives ~0, clean PSK ~1. The skimmer's
// receivers sit on whatever narrow signal there is (keyed carriers, one tone
// of an FSK pair), so they need cleaner PSK to open.
constexpr float SQ_OPEN = 0.55f, SQ_CLOSE = 0.40f;
constexpr float SQ_OPEN_STRICT = 0.75f, SQ_CLOSE_STRICT = 0.55f;
// A plain carrier also has perfect phase steps but never reverses.
constexpr float REV_MIN = 0.08f, REV_MIN_STRICT = 0.15f;
// QPSK steps by 90 degrees about half the time; BPSK practically never.
constexpr float QUAD_MIN = 0.15f;
// Speed: a faster demodulator on a slower signal also sees clean phase steps
// (each slow symbol looks like several equal fast ones), so the quality alone
// does not tell the speeds apart. Only the right speed has a deep minimum in
// the timing phases, where every reversal crosses zero: score = quality x
// contrast of the phase energies. Another mode must beat the shown one by
// SWITCH_MARGIN.
constexpr float SWITCH_MARGIN = 0.1f;
constexpr float QUALITY_DROP = 0.15f;

// AFC: share of the error corrected per block, from the mode shown.
constexpr float AFC_GAIN = 0.3f, AFC_GAIN_SEARCH = 0.2f;
constexpr float AFC_SEARCH_HZ = 25.0f;    // from the tone the analyzer set

// PSK31 Varicode for ASCII 0..127 (checked against fldigi's table).
const char *const VARICODE[128] = {
    "1010101011", "1011011011", "1011101101", "1101110111", "1011101011", "1101011111", "1011101111", "1011111101",
    "1011111111", "11101111",   "11101",      "1101101111", "1011011101", "11111",      "1101110101", "1110101011",
    "1011110111", "1011110101", "1110101101", "1110101111", "1101011011", "1101101011", "1101101101", "1101010111",
    "1101111011", "1101111101", "1110110111", "1101010101", "1101011101", "1110111011", "1011111011", "1101111111",
    "1",          "111111111",  "101011111",  "111110101",  "111011011",  "1011010101", "1010111011", "101111111",
    "11111011",   "11110111",   "101101111",  "111011111",  "1110101",    "110101",     "1010111",    "110101111",
    "10110111",   "10111101",   "11101101",   "11111111",   "101110111",  "101011011",  "101101011",  "110101101",
    "110101011",  "110110111",  "11110101",   "110111101",  "111101101",  "1010101",    "111010111",  "1010101111",
    "1010111101", "1111101",    "11101011",   "10101101",   "10110101",   "1110111",    "11011011",   "11111101",
    "101010101",  "1111111",    "111111101",  "101111101",  "11010111",   "10111011",   "11011101",   "10101011",
    "11010101",   "111011101",  "10101111",   "1101111",    "1101101",    "101010111",  "110110101",  "101011101",
    "101110101",  "101111011",  "1010101101", "111110111",  "111101111",  "111111011",  "1010111111", "101101101",
    "1011011111", "1011",       "1011111",    "101111",     "101101",     "11",         "111101",     "1011011",
    "101011",     "1101",       "111101011",  "10111111",   "11011",      "111011",     "1111",       "111",
    "111111",     "110111111",  "10101",      "10111",      "101",        "110111",     "1111011",    "1101011",
    "11011111",   "1011101",    "111010101",  "1010110111", "110111011",  "1010110101", "1011010111", "1110110101",
};
uint16_t vc_value[128];    // codes as binary numbers (leading 1)

void varicode_init()
{
    for (int c = 0; c < 128; c++) {
        uint16_t v = 0;
        for (const char *p = VARICODE[c]; *p; p++)
            v = (uint16_t)((v << 1) | (*p == '1'));
        vc_value[c] = v;
    }
}

char varicode_char(uint32_t code)
{
    for (int c = 0; c < 128; c++) {
        if (vc_value[c] == code)
            return (char)c;
    }
    return 0;
}

// Varicode bit stream -> characters ("00" ends a character).
struct VaricodeRx {
    uint32_t code = 0;
    char out[48];
    int out_len = 0;

    void bit(int b)
    {
        code = (code << 1) | (uint32_t)b;
        if ((code & 3) == 0) {
            const char c = varicode_char(code >> 2);
            if (c && out_len < (int)sizeof(out))
                out[out_len++] = c;
            code = 0;
        } else if (code > 0xFFF) {
            code = 0;    // longer than any code: lost sync
        }
    }
};

// QPSK: rate 1/2, K = 5 convolutional code (polynomials 0x17 and 0x19, as
// fldigi). Each pair of coded bits s (bit 0 from 0x17, bit 1 from 0x19) is
// sent as a phase step of 180 + 90 x ((4 - s) & 3) degrees.
constexpr int K = 5, NSTATES = 1 << (K - 1);
constexpr int VIT_DELAY = 40;    // decision depth, bits (8 constraint lengths)
uint8_t conv_out[1 << K];
float step_r[4], step_i[4];      // unit phase step of each coded pair

int parity(unsigned v)
{
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

void conv_init()
{
    for (int i = 0; i < (1 << K); i++)
        conv_out[i] = (uint8_t)(parity(0x17 & i) | (parity(0x19 & i) << 1));
    for (int s = 0; s < 4; s++) {
        const float a = (float)M_PI + (float)M_PI_2 * ((4 - s) & 3);
        step_r[s] = cosf(a);
        step_i[s] = sinf(a);
    }
}

// Register-exchange Viterbi decoder; the state's low bit is the newest input bit.
struct Viterbi {
    float metric[NSTATES];
    uint64_t path[NSTATES];
    int steps = 0;
    float fit = 0.0f;    // mean metric gain per step relative to the best possible

    void reset()
    {
        memset(metric, 0, sizeof(metric));
        memset(path, 0, sizeof(path));
        steps = 0;
        fit = 0.0f;
    }

    // bm[s]: how well the received step matches coded pair s (-1..1).
    // Returns the decoded bit VIT_DELAY steps back, or -1 while filling.
    int step(const float *bm)
    {
        float nm[NSTATES];
        uint64_t np[NSTATES];
        float best = -1e30f, prev_best = -1e30f;
        int best_state = 0;
        for (int n = 0; n < NSTATES; n++)
            prev_best = fmaxf(prev_best, metric[n]);
        for (int n = 0; n < NSTATES; n++) {
            const int p0 = n >> 1, p1 = (n >> 1) | (NSTATES >> 1);
            const float m0 = metric[p0] + bm[conv_out[n]];
            const float m1 = metric[p1] + bm[conv_out[n | NSTATES]];
            if (m0 >= m1) {
                nm[n] = m0;
                np[n] = (path[p0] << 1) | (uint64_t)(n & 1);
            } else {
                nm[n] = m1;
                np[n] = (path[p1] << 1) | (uint64_t)(n & 1);
            }
            if (nm[n] > best) {
                best = nm[n];
                best_state = n;
            }
        }
        float bm_max = bm[0];
        for (int s = 1; s < 4; s++)
            bm_max = fmaxf(bm_max, bm[s]);
        if (bm_max > 0.0f)
            fit += VIT_ALPHA * ((best - prev_best) / bm_max - fit);
        for (int n = 0; n < NSTATES; n++) {
            metric[n] = nm[n] - best;
            path[n] = np[n];
        }
        if (++steps <= VIT_DELAY)
            return -1;
        return (int)((path[best_state] >> VIT_DELAY) & 1);
    }
};

void *alloc_big(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);    // the internal RAM is short
#else
    void *p = malloc(n);
#endif
    if (p)
        memset(p, 0, n);
    return p;
}

// One speed: matched filter, symbol timing and the BPSK and QPSK decisions.
struct Demod {
    int sps = 0, nbins = 0, bin_step = 0;
    float baud = 0.0f;
    bool qpsk = false;          // also decode QPSK
    float *ring_i = nullptr, *ring_q = nullptr;
    int rpos = 0;
    float acc_i = 0.0f, acc_q = 0.0f;
    int cnt = 0;
    float bin_e[NBINS_MAX];
    int best = 0;
    float prev_i = 0.0f, prev_q = 0.0f;
    bool have_prev = false;
    float q2 = 0.0f, q4 = 0.0f;         // mean cos(2 x step), cos(4 x step)
    float rev = 0.0f, quad = 0.0f;      // share of reversals, of quarter steps
    float ferr2 = 0.0f, ferr4 = 0.0f;   // frequency error assuming BPSK, QPSK
    VaricodeRx bpsk;
    Viterbi vit[2];                     // QPSK as received, and with the steps mirrored (LSB)
    VaricodeRx qrx[2];

    void setup(float b, bool with_qpsk)
    {
        baud = b;
        qpsk = with_qpsk;
        sps = (int)(FS / b + 0.5f);
        nbins = sps % 16 == 0 ? 16 : 12;
        bin_step = sps / nbins;
        ring_i = (float *)alloc_big(sps * sizeof(float));
        ring_q = (float *)alloc_big(sps * sizeof(float));
    }

    void reset()
    {
        memset(ring_i, 0, sps * sizeof(float));
        memset(ring_q, 0, sps * sizeof(float));
        memset(bin_e, 0, sizeof(bin_e));
        rpos = cnt = best = 0;
        acc_i = acc_q = prev_i = prev_q = 0.0f;
        q2 = q4 = rev = quad = ferr2 = ferr4 = 0.0f;
        have_prev = false;
        bpsk = VaricodeRx();
        for (int k = 0; k < 2; k++) {
            vit[k].reset();
            qrx[k] = VaricodeRx();
        }
    }

    void clear_out()
    {
        bpsk.out_len = 0;
        qrx[0].out_len = qrx[1].out_len = 0;
    }

    void symbol()
    {
        const float zi = acc_i, zq = acc_q;
        if (have_prev) {
            // d = z * conj(prev): its angle is the phase step of this symbol.
            const float dr = zi * prev_i + zq * prev_q;
            const float di = zq * prev_i - zi * prev_q;
            const float m2 = dr * dr + di * di;
            if (m2 > 1e-20f) {
                const float d2r = dr * dr - di * di, d2i = 2.0f * dr * di;
                q2 += Q_ALPHA * (d2r / m2 - q2);
                // Distance of the step from the nearest 0 / 180 degrees.
                ferr2 += FERR_ALPHA * (0.5f * atan2f(d2i, d2r) / (2.0f * (float)M_PI) * baud - ferr2);
                const float d4r = d2r * d2r - d2i * d2i, d4i = 2.0f * d2r * d2i;
                q4 += Q_ALPHA * (d4r / (m2 * m2) - q4);
                ferr4 += FERR_ALPHA * (0.25f * atan2f(d4i, d4r) / (2.0f * (float)M_PI) * baud - ferr4);
                quad += REV_ALPHA * ((d2r < 0.0f ? 1.0f : 0.0f) - quad);
                if (qpsk) {
                    const float g = 1.0f / sqrtf(m2);
                    const float ur = dr * g, ui = di * g;
                    float bm[2][4];
                    for (int s = 0; s < 4; s++) {
                        bm[0][s] = ur * step_r[s] + ui * step_i[s];
                        bm[1][s] = ur * step_r[s] - ui * step_i[s];    // mirrored step
                    }
                    for (int k = 0; k < 2; k++) {
                        const int b = vit[k].step(bm[k]);
                        if (b >= 0)
                            qrx[k].bit(b);
                    }
                }
            }
            const int bit = dr > 0.0f ? 1 : 0;    // no reversal = 1
            rev += REV_ALPHA * ((bit ? 0.0f : 1.0f) - rev);
            bpsk.bit(bit);
        }
        prev_i = zi;
        prev_q = zq;
        have_prev = true;
    }

    float contrast() const
    {
        float lo = bin_e[0], hi = bin_e[0];
        for (int k = 1; k < nbins; k++) {
            lo = fminf(lo, bin_e[k]);
            hi = fmaxf(hi, bin_e[k]);
        }
        return hi > 1e-20f ? (hi - lo) / hi : 0.0f;
    }

    float score_bpsk() const { return q2 > 0.0f && rev > REV_MIN * 0.5f ? q2 * contrast() : 0.0f; }
    float score_qpsk() const { return qpsk && q4 > 0.0f && quad > QUAD_MIN * 0.5f ? q4 * contrast() : 0.0f; }

    void push(float bi, float bq)
    {
        acc_i += bi - ring_i[rpos];
        acc_q += bq - ring_q[rpos];
        ring_i[rpos] = bi;
        ring_q[rpos] = bq;
        if (++rpos == sps)
            rpos = 0;
        if (++cnt == sps) {
            cnt = 0;
            int b = 0;
            for (int k = 1; k < nbins; k++) {
                if (bin_e[k] > bin_e[b])
                    b = k;
            }
            best = b;
        }
        if (cnt % bin_step == 0) {
            const int b = cnt / bin_step;
            bin_e[b] += BIN_ALPHA * (acc_i * acc_i + acc_q * acc_q - bin_e[b]);
            if (b == best)
                symbol();
        }
    }
};

}    // namespace

struct PskReceiver::State {
    int ndemods = 0;
    Demod demods[NSPEEDS];
    float tone = 0.0f, tone_set = 0.0f;
    float osc_r = 1.0f, osc_i = 0.0f, rot_r = 1.0f, rot_i = 0.0f;
    int shown = 0;
    bool shown_qpsk = false;
    int shown_dir = 0;          // QPSK: 0 as received, 1 mirrored
    bool active = false;
    float sq_open = SQ_OPEN, sq_close = SQ_CLOSE, rev_min = REV_MIN;
    char text[PSK_TEXT_MAX];
    size_t text_len = 0;

    void retune(float hz)
    {
        tone = hz;
        const float w = 2.0f * (float)M_PI * hz / FS;
        rot_r = cosf(w);
        rot_i = -sinf(w);
    }

    float score(int m, bool q) const { return q ? demods[m].score_qpsk() : demods[m].score_bpsk(); }
    float quality(int m, bool q) const { return q ? demods[m].q4 : demods[m].q2; }
};

PskReceiver::PskReceiver(bool all_modes)
{
    static bool tables = false;
    if (!tables) {
        varicode_init();
        conv_init();
        tables = true;
    }
    void *mem = alloc_big(sizeof(State));
    s = new (mem) State();
    s->ndemods = all_modes ? NSPEEDS : 1;
    if (!all_modes) {
        s->sq_open = SQ_OPEN_STRICT;
        s->sq_close = SQ_CLOSE_STRICT;
        s->rev_min = REV_MIN_STRICT;
    }
    for (int m = 0; m < s->ndemods; m++)
        s->demods[m].setup(SPEED_BAUD[m], all_modes);
    set_tone(0.0f);
}

void PskReceiver::set_tone(float hz)
{
    if (hz == s->tone_set && hz > 0.0f)
        return;
    s->tone_set = hz;
    s->osc_r = 1.0f;
    s->osc_i = 0.0f;
    for (int m = 0; m < s->ndemods; m++)
        s->demods[m].reset();
    s->shown = 0;
    s->shown_qpsk = false;
    s->shown_dir = 0;
    s->active = false;
    s->retune(hz);
}

float PskReceiver::tone_hz() const { return s->tone_set > 0.0f ? s->tone : 0.0f; }
float PskReceiver::baud() const { return SPEED_BAUD[s->shown]; }
const char *PskReceiver::mode_name() const { return (s->shown_qpsk ? QPSK_NAME : BPSK_NAME)[s->shown]; }
float PskReceiver::quality() const
{
    const Demod &d = s->demods[s->shown];
    return s->shown_qpsk ? d.q4 : d.q2;
}
bool PskReceiver::active() const { return s->active; }

void PskReceiver::process(const float *x, int n)
{
    State &st = *s;
    if (st.tone_set <= 0.0f)
        return;
    for (int i = 0; i < n; i++) {
        const float bi = x[i] * st.osc_r, bq = x[i] * st.osc_i;
        const float r = st.osc_r * st.rot_r - st.osc_i * st.rot_i;
        st.osc_i = st.osc_r * st.rot_i + st.osc_i * st.rot_r;
        st.osc_r = r;
        for (int m = 0; m < st.ndemods; m++)
            st.demods[m].push(bi, bq);
    }
    const float g = 1.0f / sqrtf(st.osc_r * st.osc_r + st.osc_i * st.osc_i);    // keep |osc| = 1
    st.osc_r *= g;
    st.osc_i *= g;

    // Mode shown: the best score, with some hysteresis. A slower demodulator
    // on a faster signal can reach a fair score in a stretch without reversals
    // but its phase decisions are poor: it must not take over from a mode
    // whose decisions are clearly better.
    int best = st.shown;
    bool best_q = st.shown_qpsk;
    const float cur_q = st.quality(best, best_q);
    for (int m = 0; m < st.ndemods; m++) {
        for (int q = 0; q < 2; q++) {
            if (st.score(m, q) > st.score(best, best_q) + SWITCH_MARGIN &&
                st.quality(m, q) > cur_q - QUALITY_DROP) {
                best = m;
                best_q = q;
            }
        }
    }
    st.shown = best;
    st.shown_qpsk = best_q;
    Demod &d = st.demods[st.shown];
    if (st.shown_qpsk) {
        // Direction: the Viterbi path that fits better.
        if (d.vit[1 - st.shown_dir].fit > d.vit[st.shown_dir].fit + 0.05f)
            st.shown_dir = 1 - st.shown_dir;
        st.active = st.active ? (d.q4 > st.sq_close && d.quad > QUAD_MIN * 0.5f)
                              : (d.q4 > st.sq_open && d.quad > QUAD_MIN);
    } else {
        st.active = st.active ? (d.q2 > st.sq_close && d.rev > st.rev_min * 0.5f)
                              : (d.q2 > st.sq_open && d.rev > st.rev_min);
    }

    if (st.active) {
        const VaricodeRx &v = st.shown_qpsk ? d.qrx[st.shown_dir] : d.bpsk;
        for (int k = 0; k < v.out_len && st.text_len < sizeof(st.text); k++) {
            const char c = v.out[k];
            if (c == '\n' || c == ' ' || (c >= 0x20 && c < 0x7F))
                st.text[st.text_len++] = c;
            else if (c == '\r')
                st.text[st.text_len++] = '\n';
        }
    }
    // AFC from the mode shown (the best so far while searching), within reach
    // of the set tone.
    const float gain = st.active ? AFC_GAIN : AFC_GAIN_SEARCH;
    const float reach = fmaxf(AFC_SEARCH_HZ, d.baud * 0.4f);
    float f = st.tone + gain * (st.shown_qpsk ? d.ferr4 : d.ferr2);
    if (f > st.tone_set + reach)
        f = st.tone_set + reach;
    if (f < st.tone_set - reach)
        f = st.tone_set - reach;
    st.retune(f);
    for (int m = 0; m < st.ndemods; m++) {
        st.demods[m].ferr2 *= 1.0f - gain;
        st.demods[m].ferr4 *= 1.0f - gain;
        st.demods[m].clear_out();
    }
}

size_t PskReceiver::take_text(char *buf, size_t size)
{
    if (size == 0)
        return 0;
    const size_t n = s->text_len < size - 1 ? s->text_len : size - 1;
    memcpy(buf, s->text, n);
    buf[n] = 0;
    s->text_len = 0;
    return n;
}

// ---------------------------------------------------------------------------
// The main decoder.

static PskReceiver *main_rx;

void psk_init()
{
    if (!main_rx)
        main_rx = new PskReceiver(true);
}

void psk_set_tone(float hz) { main_rx->set_tone(hz); }
float psk_tone_hz() { return main_rx->tone_hz(); }
float psk_baud() { return main_rx->baud(); }
const char *psk_mode_name() { return main_rx->mode_name(); }
float psk_quality() { return main_rx->quality(); }
bool psk_active() { return main_rx->active(); }
void psk_process(const float *x, int n) { main_rx->process(x, n); }
size_t psk_take_text(char *buf, size_t size) { return main_rx->take_text(buf, size); }

#if PSK_SELFTEST

#include "esp_log.h"

void psk_selftest()
{
    static const char *const MSG = "CQ CQ DE CT1ABC CT1ABC PSE K\n";
    static float block[FFT_SIZE];
    struct Case { int speed; bool qpsk, lsb; float noise; };
    static const Case CASES[] = {
        { 0, false, false, 0.1f }, { 1, false, false, 0.1f }, { 2, false, false, 0.1f },
        { 3, false, false, 0.05f }, { 4, false, false, 0.03f },
        { 0, true, false, 0.08f }, { 1, true, false, 0.08f }, { 2, true, false, 0.05f },
        { 0, true, true, 0.08f }, { 3, true, false, 0.03f },
    };
    for (const Case &c : CASES) {
        const float baud = SPEED_BAUD[c.speed];
        const int sps = (int)(FS / baud + 0.5f);
        const float f0 = 1000.0f, offset = 4.0f;    // decoder set 4 Hz off
        // Bits: idle (zeros), the message twice, idle (the Viterbi delay).
        static uint8_t bits[2048];
        int nb = 0;
        for (int k = 0; k < 64; k++)
            bits[nb++] = 0;
        for (int rep = 0; rep < 2; rep++) {
            for (const char *p = MSG; *p; p++) {
                for (const char *b = VARICODE[(int)*p]; *b && nb < 1900; b++)
                    bits[nb++] = *b == '1';
                bits[nb++] = 0;
                bits[nb++] = 0;
            }
        }
        for (int k = 0; k < 96; k++)
            bits[nb++] = 0;

        psk_set_tone(0.0f);
        psk_set_tone(f0);
        char got[256] = "";
        size_t got_len = 0;
        char mode_seen[8] = "";    // mode while the text came
        uint32_t rnd = 1;
        float phase = 0.0f;
        float sym_r = 1.0f, sym_i = 0.0f;    // carrier phase of the current symbol
        unsigned shreg = 0;
        int pos = 0;
        for (int s = 0; s < nb; s++) {
            // Phase step of this symbol.
            float step;
            if (c.qpsk) {
                shreg = (shreg << 1) | bits[s];
                const int cs = conv_out[shreg & 31];
                step = (float)M_PI + (float)M_PI_2 * ((4 - cs) & 3);
                if (c.lsb)
                    step = -step;
            } else {
                step = bits[s] ? 0.0f : (float)M_PI;    // a 0 reverses
            }
            const float new_r = sym_r * cosf(step) - sym_i * sinf(step);
            const float new_i = sym_r * sinf(step) + sym_i * cosf(step);
            for (int i = 0; i < sps; i++) {
                // Raised-cosine transition from the last symbol to this one.
                const float w = 0.5f + 0.5f * cosf((float)M_PI * i / sps);
                const float ar = w * sym_r + (1.0f - w) * new_r;
                const float ai = w * sym_i + (1.0f - w) * new_i;
                phase += 2.0f * (float)M_PI * (f0 + offset) / FS;
                if (phase > 2.0f * (float)M_PI)
                    phase -= 2.0f * (float)M_PI;
                float noise = 0.0f;
                for (int j = 0; j < 4; j++) {
                    rnd = rnd * 1664525u + 1013904223u;
                    noise += (float)(int32_t)rnd / 2147483648.0f;
                }
                block[pos++] = 0.1f * (ar * cosf(phase) - ai * sinf(phase)) + c.noise * noise;
                if (pos == FFT_SIZE) {
                    psk_process(block, FFT_SIZE);
                    pos = 0;
                    char tmp[PSK_TEXT_MAX + 1];
                    const size_t k = psk_take_text(tmp, sizeof(tmp));
                    if (k)
                        strlcpy(mode_seen, psk_mode_name(), sizeof(mode_seen));
#ifdef PSK_DEBUG
                    printf("  %s q %.2f act %d k %d\n", psk_mode_name(), psk_quality(), psk_active(), (int)k);
#endif
                    if (got_len + k < sizeof(got)) {
                        memcpy(got + got_len, tmp, k);
                        got_len += k;
                        got[got_len] = 0;
                    }
                }
            }
            sym_r = new_r;
            sym_i = new_i;
        }
        for (char *p = got; *p; p++)
            if (*p == '\n')
                *p = '|';
        const char *want = (c.qpsk ? QPSK_NAME : BPSK_NAME)[c.speed];
        ESP_LOGW("PSK", "autoteste %s%s ruido %.2f: modo %s q %.2f tom %.1f Hz -> \"%s\" %s", want,
                 c.lsb ? " LSB" : "", c.noise, mode_seen, psk_quality(), psk_tone_hz(), got,
                 strstr(got, "CQ CQ DE CT1ABC CT1ABC PSE K") && strcmp(mode_seen, want) == 0 ? "OK" : "FALHOU");
    }
    psk_set_tone(0.0f);
}

#endif
