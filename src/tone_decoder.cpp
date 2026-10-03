#include "tone_decoder.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_timer.h"

// The project builds with -Og; these run on every sample.
#pragma GCC optimize("O2")

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;

// ---------------------------------------------------------------------------
// DTMF

constexpr int DTMF_N = DSP_SAMPLE_RATE / 40;    // 25 ms blocks
const float DTMF_HZ[8] = { 697, 770, 852, 941, 1209, 1336, 1477, 1633 };
const char DTMF_KEYS[4][5] = { "123A", "456B", "789C", "*0#D" };
// Share of the block energy: the two tones together, each one, and how far
// above the others of its group (the second strongest at most 1/6, 8 dB).
constexpr float DTMF_PAIR_MIN = 0.6f, DTMF_TONE_MIN = 0.08f, DTMF_OTHERS = 1.0f / 6.0f;
constexpr float DTMF_MIN_POWER = 1e-8f;           // mean x^2: about -80 dBFS
constexpr int64_t DTMF_SEQ_END_US = 1500000;      // a pause this long ends the line

float dtmf_coeff[8];
float dg_s1[8], dg_s2[8];
float dtmf_energy = 0.0f;
int dtmf_pos = 0;
char dtmf_prev = 0, dtmf_reported = 0;
int dtmf_quiet = 0;
bool dtmf_in_seq = false;
int64_t dtmf_last_t = 0;
char dtmf_seq[48] = "";
int dtmf_seq_len = 0;

// ---------------------------------------------------------------------------
// CTCSS

constexpr int CT_DECIM = DSP_SAMPLE_RATE / 1000;    // to 1 kHz
static_assert(DSP_SAMPLE_RATE % 1000 == 0, "CTCSS decimation needs a whole factor");
constexpr int CT_N = 1000;                          // 1 s window
constexpr int CT_HOP = 500;                         // a decision every 0.5 s
constexpr int CT_TONES = 50;
const float CT_HZ[CT_TONES] = {
    67.0f,  69.3f,  71.9f,  74.4f,  77.0f,  79.7f,  82.5f,  85.4f,  88.5f,  91.5f,
    94.8f,  97.4f,  100.0f, 103.5f, 107.2f, 110.9f, 114.8f, 118.8f, 123.0f, 127.3f,
    131.8f, 136.5f, 141.3f, 146.2f, 151.4f, 156.7f, 159.8f, 162.2f, 165.5f, 167.9f,
    171.3f, 173.8f, 177.3f, 179.9f, 183.5f, 186.2f, 189.9f, 192.8f, 196.6f, 199.5f,
    203.5f, 206.5f, 210.7f, 218.1f, 225.7f, 229.1f, 233.6f, 241.8f, 250.3f, 254.1f,
};
// The tone's share of the (Hann-windowed) energy under 300 Hz, and how far
// above the next strongest standard tone.
constexpr float CT_SHARE_MIN = 0.35f, CT_NEXT_MAX = 0.25f;
constexpr float CT_MIN_POWER = 1e-9f;

struct Biquad {
    float b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    float run(float x)
    {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};
Biquad ct_lp[2];
float ct_acc = 0.0f;
int ct_phase = 0;
EXT_RAM_BSS_ATTR float ct_ring[CT_N];
int ct_pos = 0, ct_new = 0;
float ct_hann[CT_N / 2];
float ct_coeff[CT_TONES];
int ct_cand = -1;         // tone of the last window
int ct_now = -1;          // tone held (reported)
int ct_misses = 0;

// ---------------------------------------------------------------------------
// DCS

constexpr float DCS_BAUD = 134.4f;
constexpr float DCS_PLL_GAIN = 0.15f;
constexpr int DCS_REPS = 3;              // the word repeated this many times, 23 bits apart
constexpr int DCS_LOST_BITS = 3 * 23;    // no valid word this long: released
// Standard codes (octal), as radios list them.
const uint16_t DCS_STD[] = {
    0023, 0025, 0026, 0031, 0032, 0036, 0043, 0047, 0051, 0053, 0054, 0065, 0071, 0072, 0073, 0074, 0112, 0114,
    0115, 0116, 0122, 0125, 0131, 0132, 0134, 0143, 0145, 0152, 0155, 0156, 0162, 0165, 0172, 0174, 0205, 0212,
    0223, 0225, 0226, 0243, 0244, 0245, 0246, 0251, 0252, 0255, 0261, 0263, 0265, 0266, 0271, 0274, 0306, 0311,
    0315, 0325, 0331, 0332, 0343, 0346, 0351, 0356, 0364, 0365, 0371, 0411, 0412, 0413, 0423, 0431, 0432, 0445,
    0446, 0452, 0454, 0455, 0462, 0464, 0465, 0466, 0503, 0506, 0516, 0523, 0526, 0532, 0546, 0565, 0606, 0612,
    0624, 0627, 0631, 0632, 0654, 0662, 0664, 0703, 0712, 0723, 0731, 0732, 0734, 0743, 0754,
};

// The 11 Golay parity bits of a 9-bit code (C0..C8), as documented for DCS.
uint32_t dcs_parity(uint32_t c)
{
    auto par = [](uint32_t v) { return (uint32_t)__builtin_parity(v); };
    uint32_t p = 0;
    p |= par(c & 0b010011111) << 0;
    p |= (par(c & 0b100111110) ^ 1) << 1;
    p |= par(c & 0b011100011) << 2;
    p |= (par(c & 0b111000110) ^ 1) << 3;
    p |= (par(c & 0b100010011) ^ 1) << 4;
    p |= (par(c & 0b010111001) ^ 1) << 5;
    p |= par(c & 0b111101101) << 6;
    p |= par(c & 0b111011010) << 7;
    p |= par(c & 0b110110100) << 8;
    p |= (par(c & 0b101101000) ^ 1) << 9;
    p |= (par(c & 0b001001111) ^ 1) << 10;
    return p;
}

// Word bits in transmission order from bit 0: C0..C8, filler 0 0 1, P0..P10.
uint32_t dcs_word(uint32_t code) { return code | 4u << 9 | dcs_parity(code) << 12; }

bool dcs_standard(uint32_t code)
{
    for (uint16_t c : DCS_STD)
        if (c == code)
            return true;
    return false;
}

struct DcsCand { int code = -1; bool inv = false; uint32_t last = 0; int reps = 0; };
DcsCand dcs_cand[4];
float dcs_phase = 0.0f, dcs_mean = 0.0f;
bool dcs_prev = false;
uint32_t dcs_sr = 0, dcs_bits = 0, dcs_last_valid = 0;
int dcs_now = -1;
bool dcs_now_inv = false;
char dcs_pending[48] = "", dcs_reported[48] = "";
uint32_t dcs_pending_since = 0;

EXT_RAM_BSS_ATTR char text[TONES_TEXT_MAX];
size_t text_len = 0;

void append(const char *s)
{
    while (*s && text_len < sizeof(text))
        text[text_len++] = *s++;
}

void append_stamp()
{
    const time_t t = time(nullptr);
    if (t < 1704067200)
        return;
    struct tm tm;
    gmtime_r(&t, &tm);
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d:%02dZ ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    append(b);
}

void lowpass_design(Biquad &q, float hz, float Q)
{
    const float w0 = 2.0f * (float)M_PI * hz / FS;
    const float c = cosf(w0), alpha = sinf(w0) / (2.0f * Q), a0 = 1.0f + alpha;
    q.b0 = (1.0f - c) / 2.0f / a0;
    q.b1 = (1.0f - c) / a0;
    q.b2 = q.b0;
    q.a1 = -2.0f * c / a0;
    q.a2 = (1.0f - alpha) / a0;
}

void dtmf_block()
{
    float f[8];
    const float e = dtmf_energy > 1e-20f ? dtmf_energy : 1e-20f;
    for (int i = 0; i < 8; i++) {
        const float p = dg_s1[i] * dg_s1[i] + dg_s2[i] * dg_s2[i] - dtmf_coeff[i] * dg_s1[i] * dg_s2[i];
        f[i] = 2.0f * p / (DTMF_N * e);
        dg_s1[i] = dg_s2[i] = 0.0f;
    }
    int r = 0, c = 4;
    for (int i = 1; i < 4; i++)
        if (f[i] > f[r])
            r = i;
    for (int i = 5; i < 8; i++)
        if (f[i] > f[c])
            c = i;
    bool ok = dtmf_energy / DTMF_N > DTMF_MIN_POWER && f[r] + f[c] > DTMF_PAIR_MIN && f[r] > DTMF_TONE_MIN &&
              f[c] > DTMF_TONE_MIN;
    for (int i = 0; ok && i < 4; i++)
        if (i != r && f[i] > f[r] * DTMF_OTHERS)
            ok = false;
    for (int i = 4; ok && i < 8; i++)
        if (i != c && f[i] > f[c] * DTMF_OTHERS)
            ok = false;
    dtmf_energy = 0.0f;

    const char key = ok ? DTMF_KEYS[r][c - 4] : 0;
    const int64_t now = esp_timer_get_time();
    if (key && key == dtmf_prev && key != dtmf_reported) {    // held for two blocks
        if (!dtmf_in_seq) {
            append_stamp();
            append("DTMF ");
            dtmf_in_seq = true;
            dtmf_seq_len = 0;
        }
        const char s[2] = { key, 0 };
        append(s);
        if (dtmf_seq_len < (int)sizeof(dtmf_seq) - 1) {
            dtmf_seq[dtmf_seq_len++] = key;
            dtmf_seq[dtmf_seq_len] = 0;
        }
        dtmf_reported = key;
        dtmf_last_t = now;
    }
    if (!key) {
        if (++dtmf_quiet >= 2)
            dtmf_reported = 0;    // the same key may follow after a gap
    } else {
        dtmf_quiet = 0;
    }
    dtmf_prev = key;
    if (dtmf_in_seq && now - dtmf_last_t > DTMF_SEQ_END_US) {
        append("\n");
        dtmf_in_seq = false;
    }
}

void ctcss_window()
{
    // Hann-windowed copy of the last second, oldest first.
    EXT_RAM_BSS_ATTR static float w[CT_N];
    float e = 0.0f;
    for (int i = 0; i < CT_N; i++) {
        const float h = i < CT_N / 2 ? ct_hann[i] : ct_hann[CT_N - 1 - i];
        w[i] = ct_ring[(ct_pos + i) % CT_N] * h;
        e += w[i] * w[i];
    }
    int best = -1, second = -1;
    float pb = 0.0f, ps = 0.0f;
    if (e / CT_N > CT_MIN_POWER) {
        for (int t = 0; t < CT_TONES; t++) {
            float s1 = 0.0f, s2 = 0.0f;
            const float k = ct_coeff[t];
            for (int i = 0; i < CT_N; i++) {
                const float s0 = w[i] + k * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            const float p = s1 * s1 + s2 * s2 - k * s1 * s2;
            if (p > pb) {
                ps = pb;
                second = best;
                pb = p;
                best = t;
            } else if (p > ps) {
                ps = p;
                second = t;
            }
        }
    }
    (void)second;
    // A pure tone: |X|^2 = N/3 x its windowed energy.
    const float share = e > 0.0f ? 3.0f * pb / (CT_N * e) : 0.0f;
    const int cand = best >= 0 && share > CT_SHARE_MIN && ps < pb * CT_NEXT_MAX ? best : -1;
    if (cand >= 0 && cand == ct_cand) {
        if (cand != ct_now) {
            char b[32];
            append_stamp();
            snprintf(b, sizeof(b), "CTCSS %.1f Hz\n", CT_HZ[cand]);
            append(b);
            ct_now = cand;
        }
        ct_misses = 0;
    } else if (cand < 0 && ct_now >= 0 && ++ct_misses >= 2) {
        ct_now = -1;
    }
    ct_cand = cand;
}

void dcs_bit(int b)
{
    dcs_sr = (dcs_sr >> 1) | (uint32_t)b << 22;    // bit 0 = oldest
    dcs_bits++;
    for (int inv = 0; inv < 2; inv++) {
        const uint32_t w = inv ? (~dcs_sr & 0x7FFFFF) : dcs_sr;
        const uint32_t code = w & 0x1FF;
        if (w != dcs_word(code) || !dcs_standard(code))
            continue;
        dcs_last_valid = dcs_bits;
        DcsCand *c = nullptr;
        for (DcsCand &k : dcs_cand)
            if (k.code == (int)code && k.inv == (bool)inv)
                c = &k;
        if (!c) {    // replace the stalest
            c = &dcs_cand[0];
            for (DcsCand &k : dcs_cand)
                if (k.code < 0 || k.last < c->last)
                    c = &k;
            c->code = (int)code;
            c->inv = inv;
            c->reps = 0;
            c->last = 0;
        }
        c->reps = (c->last && dcs_bits - c->last == 23) ? c->reps + 1 : 1;
        c->last = dcs_bits;
    }
    // Every standard code holding DCS_REPS repetitions: a word and its
    // rotations or inverse can be several codes at once (e.g. 754I = 116N,
    // the same signal), so they are listed together, N first, once the set has
    // held for two words.
    DcsCand act[4];
    int na = 0;
    for (const DcsCand &k : dcs_cand)
        if (k.code >= 0 && k.reps >= DCS_REPS && dcs_bits - k.last < 2 * 23)
            act[na++] = k;
    for (int i = 1; i < na; i++) {
        const DcsCand v = act[i];
        int j = i - 1;
        while (j >= 0 && (act[j].inv > v.inv || (act[j].inv == v.inv && act[j].code > v.code))) {
            act[j + 1] = act[j];
            j--;
        }
        act[j + 1] = v;
    }
    char key[48] = "";
    int k = 0;
    for (int i = 0; i < na; i++)
        k += snprintf(key + k, sizeof(key) - k, "%s%03o%c", i ? " = " : "", act[i].code, act[i].inv ? 'I' : 'N');
    if (strcmp(key, dcs_pending)) {
        strlcpy(dcs_pending, key, sizeof(dcs_pending));
        dcs_pending_since = dcs_bits;
    }
    if (na > 0 && dcs_bits - dcs_pending_since >= 2 * 23 && strcmp(key, dcs_reported)) {
        append_stamp();
        append("DCS ");
        append(key);
        append("\n");
        strlcpy(dcs_reported, key, sizeof(dcs_reported));
        dcs_now = act[0].code;
        dcs_now_inv = act[0].inv;
    } else if (dcs_now >= 0 && dcs_bits - dcs_last_valid > DCS_LOST_BITS) {
        dcs_now = -1;
        dcs_reported[0] = 0;
        for (DcsCand &c : dcs_cand)
            c = DcsCand();
    }
}

// One sample of the 1 kHz low-passed copy.
void dcs_sample(float v)
{
    dcs_mean += 0.002f * (v - dcs_mean);    // DC, ~0.5 s
    const bool level = v - dcs_mean > 0.0f;
    dcs_phase += DCS_BAUD / 1000.0f;
    if (level != dcs_prev) {
        dcs_phase += (0.5f - dcs_phase) * DCS_PLL_GAIN;    // a transition belongs half-way between samples
        dcs_prev = level;
    }
    if (dcs_phase >= 1.0f) {
        dcs_phase -= 1.0f;
        dcs_bit(level ? 1 : 0);
    }
}

}    // namespace

void tones_init()
{
    for (int i = 0; i < 8; i++)
        dtmf_coeff[i] = 2.0f * cosf(2.0f * (float)M_PI * DTMF_HZ[i] / FS);
    lowpass_design(ct_lp[0], 300.0f, 0.5412f);
    lowpass_design(ct_lp[1], 300.0f, 1.3066f);
    for (int i = 0; i < CT_N / 2; i++)
        ct_hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (CT_N - 1));
    for (int t = 0; t < CT_TONES; t++)
        ct_coeff[t] = 2.0f * cosf(2.0f * (float)M_PI * CT_HZ[t] / 1000.0f);
}

void tones_process(const float *x, int n)
{
    for (int i = 0; i < n; i++) {
        const float v = x[i];
        // DTMF
        for (int k = 0; k < 8; k++) {
            const float s0 = v + dtmf_coeff[k] * dg_s1[k] - dg_s2[k];
            dg_s2[k] = dg_s1[k];
            dg_s1[k] = s0;
        }
        dtmf_energy += v * v;
        if (++dtmf_pos == DTMF_N) {
            dtmf_pos = 0;
            // Goertzel state is (s1, s2) = (last, previous); the power formula
            // above expects that order.
            dtmf_block();
        }
        // CTCSS: low-pass, 1 kHz, window of the last second.
        ct_acc += ct_lp[1].run(ct_lp[0].run(v));
        if (++ct_phase == CT_DECIM) {
            ct_ring[ct_pos] = ct_acc / CT_DECIM;
            dcs_sample(ct_ring[ct_pos]);
            ct_acc = 0.0f;
            ct_phase = 0;
            if (++ct_pos == CT_N)
                ct_pos = 0;
            if (++ct_new == CT_HOP) {
                ct_new = 0;
                ctcss_window();
            }
        }
    }
}

size_t tones_take_text(char *buf, size_t size)
{
    if (size == 0)
        return 0;
    const size_t n = text_len < size - 1 ? text_len : size - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    text_len = 0;
    return n;
}

float ctcss_hz() { return ct_now >= 0 ? CT_HZ[ct_now] : 0.0f; }
int dcs_code() { return dcs_now; }
bool dcs_inverted() { return dcs_now_inv; }
const char *dtmf_last() { return dtmf_seq; }
int64_t dtmf_last_us() { return dtmf_last_t; }

#if TONES_SELFTEST

#include "esp_log.h"

namespace {

uint32_t rnd = 5;
float noise(float a)
{
    float s = 0.0f;
    for (int j = 0; j < 4; j++) {
        rnd = rnd * 1664525u + 1013904223u;
        s += (float)(int32_t)rnd / 2147483648.0f;
    }
    return a * s;
}

// Runs `seconds` of signal through the decoder; gen(t) gives each sample.
template <typename F> void run(float seconds, F gen, char *got, size_t size)
{
    EXT_RAM_BSS_ATTR static float block[FFT_SIZE];
    size_t len = strlen(got);
    const int total = (int)(seconds * FS);
    int pos = 0;
    for (int s = 0; s < total; s++) {
        block[pos++] = gen(s / FS);
        if (pos == FFT_SIZE || s == total - 1) {
            tones_process(block, pos);
            pos = 0;
            char tmp[TONES_TEXT_MAX + 1];
            const size_t k = tones_take_text(tmp, sizeof(tmp));
            if (k && len + k < size) {
                memcpy(got + len, tmp, k);
                len += k;
                got[len] = 0;
            }
        }
    }
}

void log_result(const char *name, char *got, bool ok)
{
    for (char *p = got; *p; p++)
        if (*p == '\n')
            *p = '|';
    ESP_LOGW("TONES", "autoteste %s: \"%s\" %s", name, got, ok ? "OK" : "FALHOU");
}

}    // namespace

void tones_selftest()
{
    // DTMF: 80 ms tones with 80 ms gaps, the column 3 dB weaker, noise.
    {
        static const char *const SEQ = "123A#*0D";
        static const char KEYS[] = "123A456B789C*0#D";
        char got[256] = "";
        run(
            3.0f,
            [](float t) {
                const int k = (int)(t / 0.16f);
                const float in = t - k * 0.16f;
                if (k >= (int)strlen(SEQ) || in > 0.08f)
                    return noise(0.01f);
                const int idx = (int)(strchr(KEYS, SEQ[k]) - KEYS);
                return 0.2f * sinf(2.0f * (float)M_PI * DTMF_HZ[idx / 4] * t) +
                       0.14f * sinf(2.0f * (float)M_PI * DTMF_HZ[4 + idx % 4] * t) + noise(0.01f);
            },
            got, sizeof(got));
        log_result("DTMF 123A#*0D", got, strstr(got, "DTMF 123A#*0D") != nullptr);
    }
    // Voice-like: harmonics of a gliding 120-220 Hz fundamental; no digits.
    {
        char got[256] = "";
        run(
            3.0f,
            [](float t) {
                const float f0 = 120.0f + 100.0f * (0.5f + 0.5f * sinf(2.0f * (float)M_PI * 0.7f * t));
                float v = 0.0f;
                for (int h = 1; h <= 12; h++)
                    v += 0.08f / h * sinf(2.0f * (float)M_PI * f0 * h * t);
                return v + noise(0.01f);
            },
            got, sizeof(got));
        log_result("voz (sem DTMF)", got, strstr(got, "DTMF") == nullptr);
    }
    // CTCSS 88.5 Hz (its neighbours are 85.4 and 91.5) under a 1 kHz tone.
    {
        char got[256] = "";
        run(
            3.0f,
            [](float t) {
                return 0.05f * sinf(2.0f * (float)M_PI * 88.5f * t) + 0.2f * sinf(2.0f * (float)M_PI * 1000.0f * t) +
                       noise(0.01f);
            },
            got, sizeof(got));
        log_result("CTCSS 88.5", got, strstr(got, "CTCSS 88.5 Hz") != nullptr);
    }
    // CTCSS 250.3 Hz (next: 254.1 Hz), then silence: the tone is released.
    {
        char got[256] = "";
        run(
            3.0f, [](float t) { return 0.05f * sinf(2.0f * (float)M_PI * 250.3f * t) + noise(0.01f); }, got,
            sizeof(got));
        const bool seen = strstr(got, "CTCSS 250.3 Hz") != nullptr;
        run(2.0f, [](float) { return noise(0.01f); }, got, sizeof(got));
        log_result("CTCSS 250.3 e silencio", got, seen && ctcss_hz() == 0.0f);
    }
    // DCS 023 (normal) and 754 (inverted) under a 1 kHz tone, then silence.
    {
        struct DcsTest { uint32_t code; bool inv; const char *expect; };
        static const DcsTest TESTS[] = { { 0023, false, "DCS 023N" }, { 0754, true, "DCS 116N = 754I" } };
        for (const DcsTest &dt : TESTS) {
            static uint32_t word;
            static bool inv;
            word = dcs_word(dt.code);
            inv = dt.inv;
            char got[256] = "";
            run(
                4.0f,
                [](float t) {
                    const int bit = (int)(t * DCS_BAUD) % 23;
                    const int b = ((word >> bit) & 1) ^ (inv ? 1 : 0);
                    return (b ? 0.05f : -0.05f) + 0.2f * sinf(2.0f * (float)M_PI * 1000.0f * t) + noise(0.01f);
                },
                got, sizeof(got));
            const bool seen = strstr(got, dt.expect) != nullptr;
            run(2.0f, [](float) { return noise(0.01f); }, got, sizeof(got));
            log_result(dt.expect, got, seen && dcs_code() < 0);
        }
    }
    // Noise only: nothing at all.
    {
        char got[256] = "";
        run(3.0f, [](float) { return noise(0.05f); }, got, sizeof(got));
        log_result("so ruido", got, got[0] == 0);
    }
}

#endif
