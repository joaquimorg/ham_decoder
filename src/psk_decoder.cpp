#include "psk_decoder.h"

// The project builds with -Og; this runs on every sample (with the other
// decoders it took the analysis over 100% of its time).
#pragma GCC optimize("O2")

#include <math.h>
#include <string.h>

#include "esp_attr.h"

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;
constexpr int NMODES = 3;
const float MODE_BAUD[NMODES] = { 31.25f, 62.5f, 125.0f };
const char *const MODE_NAME[NMODES] = { "PSK31", "PSK63", "PSK125" };
constexpr int SPS_MAX = (int)(DSP_SAMPLE_RATE / 31.25f);    // 384 at 12 kHz
static_assert(DSP_SAMPLE_RATE % 125 == 0, "symbol lengths must be whole samples");

// Symbol timing: the matched-filter energy is averaged in NBINS phases of the
// symbol; the strongest phase is where a whole symbol fills the filter.
constexpr int NBINS = 16;
constexpr float BIN_ALPHA = 0.05f;

// Per-symbol averages.
constexpr float Q_ALPHA = 0.05f;      // phase quality
constexpr float REV_ALPHA = 0.05f;    // share of phase reversals
constexpr float FERR_ALPHA = 0.1f;    // frequency error
// Squelch on the quality: noise alone gives ~0, clean BPSK ~1.
constexpr float SQ_OPEN = 0.55f, SQ_CLOSE = 0.40f;
// A plain carrier also has perfect phase steps but never reverses.
constexpr float REV_MIN = 0.08f;
// Speed: a faster demodulator on a slower signal also sees clean phase steps
// (each slow symbol looks like several equal fast ones), so the quality alone
// does not tell the speeds apart. Only the right speed has a deep minimum in
// the timing phases, where every reversal crosses zero: score = quality x
// contrast of the phase energies. Another speed must beat the shown one by
// SWITCH_MARGIN.
constexpr float SWITCH_MARGIN = 0.1f;

// AFC: share of the error corrected per block. Until the squelch opens it
// comes from the fastest demodulator, whose capture range (+-baud/4 = 31 Hz)
// covers the slower speeds too.
constexpr float AFC_GAIN = 0.3f, AFC_GAIN_SEARCH = 0.2f;
constexpr float AFC_MAX_HZ = 25.0f;   // from the tone the analyzer set

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

char varicode_char(uint32_t code)
{
    for (int c = 0; c < 128; c++) {
        if (vc_value[c] == code)
            return (char)c;
    }
    return 0;
}

struct Demod {
    int sps = 0;
    int bin_step = 0;
    float ring_i[SPS_MAX], ring_q[SPS_MAX];
    int rpos = 0;
    float acc_i = 0.0f, acc_q = 0.0f;
    int cnt = 0;
    float bin_e[NBINS];
    int best = 0;
    float prev_i = 0.0f, prev_q = 0.0f;
    bool have_prev = false;
    float quality = 0.0f, rev = 0.0f, ferr = 0.0f;
    uint32_t code = 0;
    char out[48];
    int out_len = 0;

    void reset(int samples_per_symbol)
    {
        sps = samples_per_symbol;
        bin_step = sps / NBINS;
        memset(ring_i, 0, sizeof(ring_i));
        memset(ring_q, 0, sizeof(ring_q));
        memset(bin_e, 0, sizeof(bin_e));
        rpos = cnt = best = out_len = 0;
        acc_i = acc_q = prev_i = prev_q = 0.0f;
        quality = rev = ferr = 0.0f;
        have_prev = false;
        code = 0;
    }

    void symbol(float baud)
    {
        const float zi = acc_i, zq = acc_q;
        if (have_prev) {
            // d = z * conj(prev): its angle is the phase step of this symbol.
            const float dr = zi * prev_i + zq * prev_q;
            const float di = zq * prev_i - zi * prev_q;
            const float d2r = dr * dr - di * di, d2i = 2.0f * dr * di;
            const float m2 = dr * dr + di * di;
            if (m2 > 1e-20f) {
                quality += Q_ALPHA * (d2r / m2 - quality);
                // Distance of the step from the nearest 0 / 180 degrees.
                const float e = 0.5f * atan2f(d2i, d2r);
                ferr += FERR_ALPHA * (e / (2.0f * (float)M_PI) * baud - ferr);
            }
            const int bit = dr > 0.0f ? 1 : 0;    // no reversal = 1
            rev += REV_ALPHA * ((bit ? 0.0f : 1.0f) - rev);
            code = (code << 1) | bit;
            if ((code & 3) == 0) {    // "00" ends a character
                const char c = varicode_char(code >> 2);
                if (c && out_len < (int)sizeof(out))
                    out[out_len++] = c;
                code = 0;
            } else if (code > 0xFFF) {
                code = 0;    // longer than any code: lost sync
            }
        }
        prev_i = zi;
        prev_q = zq;
        have_prev = true;
    }

    float contrast() const
    {
        float lo = bin_e[0], hi = bin_e[0];
        for (int k = 1; k < NBINS; k++) {
            lo = fminf(lo, bin_e[k]);
            hi = fmaxf(hi, bin_e[k]);
        }
        return hi > 1e-20f ? (hi - lo) / hi : 0.0f;
    }

    float score() const { return quality > 0.0f ? quality * contrast() : 0.0f; }

    void push(float bi, float bq, float baud)
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
            for (int k = 1; k < NBINS; k++) {
                if (bin_e[k] > bin_e[b])
                    b = k;
            }
            best = b;
        }
        if (cnt % bin_step == 0) {
            const int b = cnt / bin_step;
            bin_e[b] += BIN_ALPHA * (acc_i * acc_i + acc_q * acc_q - bin_e[b]);
            if (b == best)
                symbol(baud);
        }
    }
};

EXT_RAM_BSS_ATTR Demod demods[NMODES];    // ~10 KB: in PSRAM, the internal RAM is short

float tone = 0.0f, tone_set = 0.0f;
float osc_r = 1.0f, osc_i = 0.0f, rot_r = 1.0f, rot_i = 0.0f;
int shown = 0;
bool active = false;

EXT_RAM_BSS_ATTR char text[PSK_TEXT_MAX];
size_t text_len = 0;

void retune(float hz)
{
    tone = hz;
    const float w = 2.0f * (float)M_PI * hz / FS;
    rot_r = cosf(w);
    rot_i = -sinf(w);
}

}    // namespace

void psk_init()
{
    for (int c = 0; c < 128; c++) {
        uint16_t v = 0;
        for (const char *p = VARICODE[c]; *p; p++)
            v = (uint16_t)((v << 1) | (*p == '1'));
        vc_value[c] = v;
    }
    psk_set_tone(0.0f);
}

void psk_set_tone(float hz)
{
    if (hz == tone_set && hz > 0.0f)
        return;
    tone_set = hz;
    osc_r = 1.0f;
    osc_i = 0.0f;
    for (int m = 0; m < NMODES; m++)
        demods[m].reset((int)(FS / MODE_BAUD[m] + 0.5f));
    shown = 0;
    active = false;
    retune(hz);
}

float psk_tone_hz() { return tone_set > 0.0f ? tone : 0.0f; }
float psk_baud() { return MODE_BAUD[shown]; }
const char *psk_mode_name() { return MODE_NAME[shown]; }
float psk_quality() { return demods[shown].quality; }
bool psk_active() { return active; }

void psk_process(const float *x, int n)
{
    if (tone_set <= 0.0f)
        return;
    for (int i = 0; i < n; i++) {
        const float bi = x[i] * osc_r, bq = x[i] * osc_i;
        const float r = osc_r * rot_r - osc_i * rot_i;
        osc_i = osc_r * rot_i + osc_i * rot_r;
        osc_r = r;
        for (int m = 0; m < NMODES; m++)
            demods[m].push(bi, bq, MODE_BAUD[m]);
    }
    const float g = 1.0f / sqrtf(osc_r * osc_r + osc_i * osc_i);    // keep |osc| = 1
    osc_r *= g;
    osc_i *= g;

    // Speed shown: the best score, with some hysteresis.
    int best = shown;
    for (int m = 0; m < NMODES; m++) {
        if (demods[m].score() > demods[best].score() + SWITCH_MARGIN)
            best = m;
    }
    shown = best;
    Demod &d = demods[shown];
    active = active ? (d.quality > SQ_CLOSE && d.rev > REV_MIN * 0.5f)
                    : (d.quality > SQ_OPEN && d.rev > REV_MIN);

    if (active) {
        for (int k = 0; k < d.out_len && text_len < sizeof(text); k++) {
            const char c = d.out[k];
            if (c == '\n' || c == ' ' || (c >= 0x20 && c < 0x7F))
                text[text_len++] = c;
            else if (c == '\r')
                text[text_len++] = '\n';
        }
    }
    // AFC within reach of the set tone.
    const Demod &src = active ? d : demods[NMODES - 1];
    const float gain = active ? AFC_GAIN : AFC_GAIN_SEARCH;
    float f = tone + gain * src.ferr;
    if (f > tone_set + AFC_MAX_HZ)
        f = tone_set + AFC_MAX_HZ;
    if (f < tone_set - AFC_MAX_HZ)
        f = tone_set - AFC_MAX_HZ;
    retune(f);
    for (int m = 0; m < NMODES; m++)
        demods[m].ferr *= 1.0f - gain;
    for (int m = 0; m < NMODES; m++)
        demods[m].out_len = 0;
}

size_t psk_take_text(char *buf, size_t size)
{
    if (size == 0)
        return 0;
    const size_t n = text_len < size - 1 ? text_len : size - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    text_len = 0;
    return n;
}

#if PSK_SELFTEST

#include <stdlib.h>
#include "esp_log.h"

void psk_selftest()
{
    static const char *const MSG = "CQ CQ DE CT1ABC CT1ABC PSE K\n";
    EXT_RAM_BSS_ATTR static float block[FFT_SIZE];
    for (int m = 0; m < NMODES; m++) {
        const float baud = MODE_BAUD[m];
        const int sps = (int)(FS / baud + 0.5f);
        const float f0 = 1000.0f, offset = 4.0f;    // decoder set 4 Hz off
        // Bits: idle reversals, the message twice, idle.
        EXT_RAM_BSS_ATTR static uint8_t bits[2048];
        int nb = 0;
        for (int k = 0; k < 64; k++)
            bits[nb++] = 0;
        for (int rep = 0; rep < 2; rep++) {
            for (const char *p = MSG; *p; p++) {
                for (const char *b = VARICODE[(int)*p]; *b && nb < 2000; b++)
                    bits[nb++] = *b == '1';
                bits[nb++] = 0;
                bits[nb++] = 0;
            }
        }
        for (int k = 0; k < 32; k++)
            bits[nb++] = 0;

        psk_set_tone(0.0f);
        psk_set_tone(f0);
        char got[256] = "";
        size_t got_len = 0;
        uint32_t rnd = 1;
        float phase = 0.0f, sign = 1.0f;
        int pos = 0;
        for (int s = 0; s < nb; s++) {
            const bool reverse = bits[s] == 0;
            for (int i = 0; i < sps; i++) {
                // Raised-cosine amplitude through a reversal, constant otherwise.
                const float a = reverse ? sign * cosf((float)M_PI * i / sps) : sign;
                phase += 2.0f * (float)M_PI * (f0 + offset) / FS;
                if (phase > 2.0f * (float)M_PI)
                    phase -= 2.0f * (float)M_PI;
                float noise = 0.0f;
                for (int j = 0; j < 4; j++) {
                    rnd = rnd * 1664525u + 1013904223u;
                    noise += (float)(int32_t)rnd / 2147483648.0f;
                }
                block[pos++] = 0.1f * a * cosf(phase) + 0.1f * noise;    // ~10 dB SNR in 3 kHz
                if (pos == FFT_SIZE) {
                    psk_process(block, FFT_SIZE);
                    pos = 0;
                    char tmp[PSK_TEXT_MAX + 1];
                    const size_t k = psk_take_text(tmp, sizeof(tmp));
                    if (got_len + k < sizeof(got)) {
                        memcpy(got + got_len, tmp, k);
                        got_len += k;
                        got[got_len] = 0;
                    }
                }
            }
            if (reverse)
                sign = -sign;
        }
        for (char *p = got; *p; p++)
            if (*p == '\n')
                *p = '|';
        ESP_LOGW("PSK", "autoteste %s: modo %s q %.2f tom %.1f Hz -> \"%s\" %s", MODE_NAME[m], psk_mode_name(),
                 psk_quality(), psk_tone_hz(), got, strstr(got, "CQ CQ DE CT1ABC CT1ABC PSE K") ? "OK" : "FALHOU");
    }
    psk_set_tone(0.0f);
}

#endif
