#include "navtex_decoder.h"

// The project builds with -Og; this runs on every sample.
#pragma GCC optimize("O2")

#include <math.h>
#include <string.h>

#include "esp_attr.h"

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;
constexpr int SPB = DSP_SAMPLE_RATE / 100;        // samples per bit (100 baud)
static_assert(DSP_SAMPLE_RATE % 100 == 0, "a bit must be a whole number of samples");
constexpr int SMOOTH = SPB / 4;

constexpr float AGC_ATTACK = 0.3f, AGC_DECAY = 0.0005f;
constexpr int32_t PLL_STEP = (int32_t)(4294967296.0 * 100.0 / DSP_SAMPLE_RATE);
constexpr float PLL_INERTIA_LOCKED = 0.8f, PLL_INERTIA_SEARCH = 0.5f;

// CCIR 476 (ITU-R M.476): codes with four marks; letters / figures.
constexpr int CODE_LTRS = 0x5A, CODE_FIGS = 0x36, CODE_ALPHA = 0x0F, CODE_REP = 0x66;
constexpr int CODE_CR = 0x78, CODE_LF = 0x6C, CODE_SPACE = 0x5C;
const char LTRS[128] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          0, 0, 0, 0, 0, 0, 0, 'J', 0, 0, 0, 'F', 0, 'C', 'K', 0,
    0, 0, 0, 0, 0, 0, 0, 'W', 0, 0, 0, 'Y', 0, 'P', 'Q', 0,  0, 0, 0, 0, 0, 'G', 0, 0, 0, 'M', 'X', 0, 'V', 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 'A', 0, 0, 0, 'S', 0, 'I', 'U', 0,  0, 0, 0, 'D', 0, 'R', 'E', 0, 0, 'N', 0, 0, ' ', 0, 0, 0,
    0, 0, 0, 'Z', 0, 'L', 0, 0, 0, 'H', 0, 0, '\n', 0, 0, 0, 0, 'O', 'B', 0, 'T', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
const char FIGS[128] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          0, 0, 0, 0, 0, 0, 0, '\'', 0, 0, 0, '!', 0, ':', '(', 0,
    0, 0, 0, 0, 0, 0, 0, '2', 0, 0, 0, '6', 0, '0', '1', 0,  0, 0, 0, 0, 0, '&', 0, 0, 0, '.', '/', 0, ';', 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '-', 0, 0, 0, 0, 0, '8', '7', 0,    0, 0, 0, '$', 0, '4', '3', 0, 0, ',', 0, 0, ' ', 0, 0, 0,
    0, 0, 0, '"', 0, ')', 0, 0, 0, '#', 0, 0, '\n', 0, 0, 0, 0, '9', '?', 0, '5', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

inline bool valid(int c) { return __builtin_popcount(c) == 4; }

// The 7 bits ending at bit 0 of sr (bit 0 = newest), as a code with the
// first-received bit in bit 0.
inline int code_at(uint64_t sr, int shift)
{
    const int v = (int)((sr >> shift) & 0x7F);
    int c = 0;
    for (int i = 0; i < 7; i++)
        if (v & (1 << i))
            c |= 1 << (6 - i);
    return c;
}

struct Tone {
    float wr = 1.0f, wi = 0.0f, cr = 1.0f, ci = 0.0f;
    float ring_r[SPB] = {}, ring_i[SPB] = {};
    float acc_r = 0.0f, acc_i = 0.0f, peak = 0.0f, valley = 0.0f;

    void tune(float hz)
    {
        const float w = 2.0f * (float)M_PI * hz / FS;
        wr = cosf(w);
        wi = -sinf(w);
        cr = 1.0f;
        ci = 0.0f;
        acc_r = acc_i = peak = valley = 0.0f;
        memset(ring_r, 0, sizeof(ring_r));
        memset(ring_i, 0, sizeof(ring_i));
    }
    float push(float x, int pos)
    {
        const float r = x * cr, i = x * ci;
        const float nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
        acc_r += r - ring_r[pos];
        acc_i += i - ring_i[pos];
        ring_r[pos] = r;
        ring_i[pos] = i;
        const float m = sqrtf(acc_r * acc_r + acc_i * acc_i);
        peak += (m > peak ? AGC_ATTACK : AGC_DECAY) * (m - peak);
        valley += (m < valley ? AGC_ATTACK : AGC_DECAY) * (m - valley);
        const float span = peak - valley;
        return span > 1e-9f ? (m - valley) / span : 0.0f;
    }
    void normalise()
    {
        const float g = 1.0f / sqrtf(cr * cr + ci * ci);
        cr *= g;
        ci *= g;
    }
};

EXT_RAM_BSS_ATTR Tone tone_hi, tone_lo;    // ~2 KB: in PSRAM, the internal RAM is short
float f_hi = 0.0f, f_lo = 0.0f;
int ring_pos = 0;
float diff_ring[SMOOTH] = {};
float diff_sum = 0.0f;
int diff_pos = 0;

int32_t pll = 0;
bool prev_level = false;
int good = 0;

uint64_t sr = 0;
uint32_t nbits = 0;

// Alignment search: for each polarity and each of the 14 bit phases at which a
// character can end, how often the character is valid and how often the phase
// looks like an alpha (main) slot.
constexpr float EMA = 1.0f / 8.0f;     // per pair of characters: ~1 s to find the alignment
float valid_ema[2][14], alpha_ema[2][14];
int pol = 0, phase = -1;      // phase at which alpha characters end, -1 = none
float quality = 0.0f;         // share of alpha slots with a usable character
bool active = false, figs = false;
constexpr float Q_OPEN = 0.8f, Q_CLOSE = 0.5f;

EXT_RAM_BSS_ATTR char text[NAVTEX_TEXT_MAX];
size_t text_len = 0;

void put(char c)
{
    if (text_len < sizeof(text))
        text[text_len++] = c;
}

void reset_sync()
{
    memset(valid_ema, 0, sizeof(valid_ema));
    memset(alpha_ema, 0, sizeof(alpha_ema));
    phase = -1;
    quality = 0.0f;
    active = false;
    figs = false;
    nbits = 0;
    sr = 0;
}

void character(int c)
{
    if (c == CODE_ALPHA || c == CODE_REP)
        return;    // phasing
    if (c == CODE_LTRS) {
        figs = false;
        return;
    }
    if (c == CODE_FIGS) {
        figs = true;
        return;
    }
    if (c == CODE_CR)
        return;
    if (c == CODE_SPACE)
        figs = false;    // unshift on space, as RTTY does
    const char ch = figs ? FIGS[c] : LTRS[c];
    if (ch)
        put(ch);
}

void bit(int b)
{
    sr = (sr << 1) | (uint64_t)b;
    nbits++;
    if (nbits < 42)
        return;
    const int a = (int)(nbits % 14);
    for (int p = 0; p < 2; p++) {
        int c = code_at(sr, 0), c35 = code_at(sr, 35);
        if (p) {
            c = ~c & 0x7F;
            c35 = ~c35 & 0x7F;
        }
        const bool v = valid(c);
        valid_ema[p][a] += EMA * ((v ? 1.0f : 0.0f) - valid_ema[p][a]);
        // Alpha evidence: the character matches the one 35 bits back (its
        // repetition), or phasing (alpha here, rep half a pair away).
        float ev = 0.0f;
        if (v && (c == c35 || c == CODE_ALPHA))
            ev = 1.0f;
        alpha_ema[p][a] += EMA * (ev - alpha_ema[p][a]);
        if (v && c == CODE_REP) {
            const int o = (a + 7) % 14;
            alpha_ema[p][o] += EMA * (1.0f - alpha_ema[p][o]);
        }
    }
    // Every pair (14 bits): pick polarity and alpha phase.
    if (a == 0) {
        // Polarity: the one with the best aligned pair. (The total of valid
        // codes over all 14 phases is no guide: during phasing the misaligned
        // phases of the wrong polarity are often "valid" too.)
        float ps[2] = { 0, 0 };
        for (int p = 0; p < 2; p++)
            for (int k = 0; k < 14; k++)
                ps[p] = fmaxf(ps[p], alpha_ema[p][k] + valid_ema[p][k] + valid_ema[p][(k + 7) % 14]);
        pol = ps[1] > ps[0] ? 1 : 0;
        int best = 0;
        for (int k = 1; k < 14; k++)
            if (alpha_ema[pol][k] > alpha_ema[pol][best])
                best = k;
        const int prev = phase;
        if (phase < 0) {
            if (alpha_ema[pol][best] > 0.3f && valid_ema[pol][best] > 0.5f) {
                phase = best;
                quality = 0.5f;    // phasing seen: open after a few good characters
            }
        } else {
            // Locked: keep the phase through bursts of damaged characters
            // (the repetitions still decode); move only to a clearly better
            // one, drop it when neither slot of the pair holds valid codes.
            const float both = valid_ema[pol][phase] + valid_ema[pol][(phase + 7) % 14];
            if (both < 0.6f)
                phase = -1;
            else if (best != phase && alpha_ema[pol][best] > alpha_ema[pol][phase] + 0.3f)
                phase = best;
        }
        if (phase != prev)
            figs = false;
    }
    if (phase < 0 || a != phase) {
        if (phase < 0)
            active = false;
        return;
    }
    // Alpha slot: the main copy, else its repetition 35 bits back.
    int c = code_at(sr, 0), c35 = code_at(sr, 35);
    if (pol) {
        c = ~c & 0x7F;
        c35 = ~c35 & 0x7F;
    }
    const int use = valid(c) ? c : valid(c35) ? c35 : -1;
    quality += 0.2f * ((use >= 0 ? 1.0f : 0.0f) - quality);    // ~1 s to open
    active = active ? quality > Q_CLOSE : quality > Q_OPEN;
    if (!active)
        return;
    if (use >= 0)
        character(use);
    else
        put('_');
}

}    // namespace

void navtex_init()
{
    navtex_set_tones(0.0f, 0.0f);
}

void navtex_set_tones(float f1, float f2)
{
    const float hi = fmaxf(f1, f2), lo = fminf(f1, f2);
    if (hi <= 0.0f) {
        f_hi = f_lo = 0.0f;
        reset_sync();
        return;
    }
    // Small retunes (AFC of the analyzer) keep the sync.
    if (fabsf(hi - f_hi) < 8.0f && fabsf(lo - f_lo) < 8.0f)
        return;
    f_hi = hi;
    f_lo = lo;
    tone_hi.tune(hi);
    tone_lo.tune(lo);
    reset_sync();
}

float navtex_mark_hz() { return active ? f_hi : (f_hi > 0.0f ? f_hi : 0.0f); }
float navtex_space_hz() { return f_lo; }
bool navtex_active() { return active; }

void navtex_process(const float *x, int n)
{
    if (f_hi <= 0.0f)
        return;
    for (int i = 0; i < n; i++) {
        const float h = tone_hi.push(x[i], ring_pos), l = tone_lo.push(x[i], ring_pos);
        if (++ring_pos == SPB)
            ring_pos = 0;
        const float d = h - l;
        diff_sum += d - diff_ring[diff_pos];
        diff_ring[diff_pos] = d;
        if (++diff_pos == SMOOTH)
            diff_pos = 0;
        const bool level = diff_sum > 0.0f;
        const int32_t before = pll;
        pll = (int32_t)((uint32_t)pll + (uint32_t)PLL_STEP);
        if (before > 0 && pll < 0)
            bit(level ? 1 : 0);
        if (level != prev_level) {
            const bool near = pll > -(PLL_STEP * 2) && pll < PLL_STEP * 2;
            good = near ? (good < 64 ? good + 1 : 64) : (good > 0 ? good - 1 : 0);
            pll = (int32_t)(pll * (good > 32 ? PLL_INERTIA_LOCKED : PLL_INERTIA_SEARCH));
            prev_level = level;
        }
    }
    tone_hi.normalise();
    tone_lo.normalise();
}

size_t navtex_take_text(char *buf, size_t size)
{
    if (size == 0)
        return 0;
    const size_t n = text_len < size - 1 ? text_len : size - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    text_len = 0;
    return n;
}

#if NAVTEX_SELFTEST

#include "esp_log.h"

namespace {

// Codes of a text (with LTRS/FIGS shifts as needed).
int encode_text(const char *s, uint8_t *out, int max)
{
    int n = 0;
    bool fig = false;
    out[n++] = CODE_LTRS;
    for (; *s && n < max - 2; s++) {
        const char ch = *s;
        if (ch == '\n') {
            out[n++] = CODE_CR;
            out[n++] = CODE_LF;
            continue;
        }
        int lc = -1, fc = -1;
        for (int c = 0; c < 128; c++) {
            if (!valid(c))
                continue;
            if (LTRS[c] == ch) lc = c;
            if (FIGS[c] == ch) fc = c;
        }
        if (lc >= 0 && (!fig || fc < 0 || ch == ' ')) {
            if (fig && ch != ' ') {
                out[n++] = CODE_LTRS;
                fig = false;
            }
            out[n++] = (uint8_t)lc;
            if (ch == ' ')
                fig = false;
        } else if (fc >= 0) {
            if (!fig) {
                out[n++] = CODE_FIGS;
                fig = true;
            }
            out[n++] = (uint8_t)fc;
        }
    }
    return n;
}

}    // namespace

void navtex_selftest()
{
    static const char *const MSG = "ZCZC EA01\nNAVAREA II 123/26\nTESTE CT1ABC 38-42N 009-08W\nNNNN\n";
    EXT_RAM_BSS_ATTR static uint8_t chars[256];
    const int nch = encode_text(MSG, chars, sizeof(chars));
    // Slots of 7 bits alternate rep / alpha; alpha k in slot 2k+1, its rep
    // 35 bits (5 slots) earlier, in slot 2k-4. Phasing first.
    const int pre = 43, post = 8;    // ~6 s of phasing (stations send ~10 s)
    const int nslots = 2 * (nch + pre + post);
    struct Case { const char *name; bool invert; float noise, offset_hz; int damage; };
    static const Case CASES[] = {
        { "limpo", false, 0.0f, 0.0f, 0 },
        { "invertido + ruido + 4 Hz", true, 0.08f, 4.0f, 0 },
        { "alpha danificados (FEC)", false, 0.02f, 0.0f, 6 },
    };
    EXT_RAM_BSS_ATTR static float block[FFT_SIZE];
    for (const Case &cs : CASES) {
        navtex_set_tones(0.0f, 0.0f);
        navtex_set_tones(1085.0f, 915.0f);
        char got[512] = "";
        size_t gl = 0;
        uint32_t rnd = 21;
        float ph = 0.0f;
        int pos = 0;
        for (int s = 0; s < nslots; s++) {
            int code;
            if (s & 1) {    // alpha slot
                const int k = (s - 1) / 2 - pre;
                code = k >= 0 && k < nch ? chars[k] : CODE_ALPHA;
                // Damage some alpha copies (their reps are intact).
                if (cs.damage && k >= 10 && k < 10 + cs.damage)
                    code ^= 0x01;
            } else {        // rep slot: character k = s/2 + 2
                const int k = s / 2 + 2 - pre;
                code = k >= 0 && k < nch ? chars[k] : CODE_REP;
            }
            for (int b = 0; b < 7; b++) {
                int bitv = (code >> b) & 1;
                if (cs.invert)
                    bitv ^= 1;
                const float f = (bitv ? 1085.0f : 915.0f) + cs.offset_hz;
                for (int k = 0; k < SPB; k++) {
                    ph += 2.0f * (float)M_PI * f / FS;
                    if (ph > 2.0f * (float)M_PI)
                        ph -= 2.0f * (float)M_PI;
                    float nz = 0.0f;
                    for (int j = 0; j < 4; j++) {
                        rnd = rnd * 1664525u + 1013904223u;
                        nz += (float)(int32_t)rnd / 2147483648.0f;
                    }
                    block[pos++] = 0.2f * sinf(ph) + cs.noise * nz;
                    if (pos == FFT_SIZE) {
                        navtex_process(block, FFT_SIZE);
                        pos = 0;
                        char tmp[NAVTEX_TEXT_MAX + 1];
                        const size_t t = navtex_take_text(tmp, sizeof(tmp));
                        if (t && gl + t < sizeof(got)) {
                            memcpy(got + gl, tmp, t);
                            gl += t;
                            got[gl] = 0;
                        }
                    }
                }
            }
        }
        for (char *p = got; *p; p++)
            if (*p == '\n')
                *p = '|';
        const bool ok = strstr(got, "ZCZC EA01") && strstr(got, "TESTE CT1ABC 38-42N 009-08W") && strstr(got, "NAVAREA II 123/26");
        ESP_LOGW("NAVTEX", "autoteste %s: \"%s\" %s", cs.name, got, ok ? "OK" : "FALHOU");
    }
    navtex_set_tones(0.0f, 0.0f);
}

#endif
