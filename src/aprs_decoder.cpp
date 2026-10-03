#include "aprs_decoder.h"

// The project builds with -Og; this runs on every sample (with the other
// decoders it took the analysis over 100% of its time).
#pragma GCC optimize("O2")

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_timer.h"

#include "aprs_format.h"
#include "settings.h"

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;
constexpr float BAUD = 1200.0f;
constexpr float MARK_HZ = 1200.0f, SPACE_HZ = 2200.0f;
constexpr int SPB = (int)(DSP_SAMPLE_RATE / 1200);    // samples per bit (10 at 12 kHz)
static_assert(DSP_SAMPLE_RATE % 1200 == 0, "a bit must be a whole number of samples");

// Tone levels: each tone has its own peak and valley follower, so the
// de-emphasis tilt of an FM receiver (2200 Hz several dB under 1200 Hz) does
// not bias the decisions.
constexpr float AGC_ATTACK = 0.3f, AGC_DECAY = 0.0003f;

// Bit clock: a DPLL that overflows once per bit; at each transition of the
// demodulated signal the phase is pulled towards the middle between samples.
constexpr float PLL_INERTIA_LOCKED = 0.74f, PLL_INERTIA_SEARCH = 0.5f;

// The mark - space difference is smoothed over SMOOTH samples (half a bit):
// in tools-side simulation (noise, de-emphasis tilt) it decoded 22-29 frames
// of 30 where the raw difference decoded 5-15.
constexpr int SMOOTH = 5;

constexpr int MIN_FRAME = 17;     // 2 addresses + control + FCS (supervisory frames)
constexpr int MAX_FRAME = 340;    // up to 8 digipeaters + 256 bytes of information

struct Tone {
    float wr = 1.0f, wi = 0.0f;    // per-sample rotation
    float cr = 1.0f, ci = 0.0f;    // oscillator
    float ring_r[SPB] = {}, ring_i[SPB] = {};
    float acc_r = 0.0f, acc_i = 0.0f;
    float peak = 0.0f, valley = 0.0f;

    void init(float hz)
    {
        const float w = 2.0f * (float)M_PI * hz / FS;
        wr = cosf(w);
        wi = -sinf(w);
    }

    // Level of the tone over the last bit, normalised between its valley and peak.
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

Tone mark, space;
int ring_pos = 0;
float diff_ring[SMOOTH] = {};
float diff_sum = 0.0f;
int diff_pos = 0;

int32_t pll = 0;
constexpr int32_t PLL_STEP = (int32_t)(4294967296.0 * 1200.0 / DSP_SAMPLE_RATE);
bool prev_level = false;     // demodulated: true = mark
bool prev_raw = false;       // for NRZI
int good_bits = 0;           // transitions near the expected place (lock indicator)

// HDLC receiver.
uint8_t pattern = 0;
bool in_frame = false;
int ones = 0;
uint8_t acc = 0;
int bitpos = 0;
EXT_RAM_BSS_ATTR uint8_t frame[MAX_FRAME];
int nbytes = 0;

EXT_RAM_BSS_ATTR char text[APRS_TEXT_MAX];
size_t text_len = 0;
uint32_t frames_ok = 0;
char last_src[12] = "";
int64_t last_us = 0;

uint16_t fcs(const uint8_t *p, int n)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return crc ^ 0xFFFF;
}

// "HH:MM:SSZ " (UTC) when the clock has been set (NTP or the web page), else "".
int utc_stamp(char *out, size_t size)
{
    const time_t t = time(nullptr);
    if (t < 1704067200)
        return 0;
    struct tm tm;
    gmtime_r(&t, &tm);
    return snprintf(out, size, "%02d:%02d:%02dZ ", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void append(const char *s)
{
    while (*s && text_len < sizeof(text))
        text[text_len++] = *s++;
}

void frame_done()
{
    if (nbytes < MIN_FRAME)
        return;
    const uint16_t got = (uint16_t)(frame[nbytes - 2] | (frame[nbytes - 1] << 8));
    if (fcs(frame, nbytes - 2) != got)
        return;
    const int len = nbytes - 2;

    // Every valid frame is shown: APRS interpreted, any other packet raw.
    char stamp[16] = "";
    utc_stamp(stamp, sizeof(stamp));
    EXT_RAM_BSS_ATTR static char line[1024];
    char src[12] = "";
    if (aprs_format(frame, len, g_settings.language == LANG_EN, stamp, line, sizeof(line), src, sizeof(src)) <= 0)
        return;
    strlcpy(last_src, src, sizeof(last_src));
    append(line);
    frames_ok++;
    last_us = esp_timer_get_time();
}

void hdlc_bit(int bit)
{
    pattern = (uint8_t)((pattern >> 1) | (bit ? 0x80 : 0));
    if (pattern == 0x7E) {    // flag: the bits since the last whole byte are its first 7
        if (in_frame && bitpos == 7)
            frame_done();
        in_frame = true;
        nbytes = 0;
        bitpos = 0;
        ones = 0;
        return;
    }
    if (bit) {
        if (++ones >= 7) {    // abort / idle
            in_frame = false;
            return;
        }
    } else {
        if (ones == 5) {      // stuffed zero
            ones = 0;
            return;
        }
        ones = 0;
    }
    if (!in_frame)
        return;
    acc = (uint8_t)((acc >> 1) | (bit ? 0x80 : 0));
    if (++bitpos == 8) {
        bitpos = 0;
        if (nbytes < MAX_FRAME)
            frame[nbytes++] = acc;
        else
            in_frame = false;
    }
}

}    // namespace

void aprs_init()
{
    mark.init(MARK_HZ);
    space.init(SPACE_HZ);
}

void aprs_process(const float *x, int n)
{
    for (int i = 0; i < n; i++) {
        const float m = mark.push(x[i], ring_pos);
        const float s = space.push(x[i], ring_pos);
        if (++ring_pos == SPB)
            ring_pos = 0;
        const float d = m - s;
        diff_sum += d - diff_ring[diff_pos];
        diff_ring[diff_pos] = d;
        if (++diff_pos == SMOOTH)
            diff_pos = 0;
        const bool level = diff_sum > 0.0f;

        const int32_t before = pll;
        pll = (int32_t)((uint32_t)pll + (uint32_t)PLL_STEP);
        if (before > 0 && pll < 0) {    // middle of a bit
            const int bit = level == prev_raw ? 1 : 0;    // NRZI: no change = 1
            prev_raw = level;
            hdlc_bit(bit);
        }
        if (level != prev_level) {
            // A transition should fall where the phase is near 0.
            const bool near = pll > -(PLL_STEP * 2) && pll < PLL_STEP * 2;
            good_bits = near ? (good_bits < 64 ? good_bits + 1 : 64) : (good_bits > 0 ? good_bits - 1 : 0);
            pll = (int32_t)(pll * (good_bits > 32 ? PLL_INERTIA_LOCKED : PLL_INERTIA_SEARCH));
            prev_level = level;
        }
    }
    mark.normalise();
    space.normalise();
}

size_t aprs_take_text(char *buf, size_t size)
{
    if (size == 0)
        return 0;
    const size_t n = text_len < size - 1 ? text_len : size - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    text_len = 0;
    return n;
}

uint32_t aprs_frames() { return frames_ok; }
const char *aprs_last_source() { return last_src; }
int64_t aprs_last_us() { return last_us; }

#if APRS_SELFTEST

#include <stdlib.h>
#include "esp_log.h"

namespace {

// Builds an AX.25 UI frame (with FCS) from calls and information.
int build_frame(uint8_t *out, const char *const *calls, int ncalls, const char *info)
{
    int n = 0;
    for (int a = 0; a < ncalls; a++) {
        const char *c = calls[a];
        int ssid = 0;
        char base[7] = "      ";
        int k = 0;
        for (; *c && *c != '-' && k < 6; c++)
            base[k++] = *c;
        if (*c == '-')
            ssid = atoi(c + 1);
        for (int i = 0; i < 6; i++)
            out[n++] = (uint8_t)(base[i] << 1);
        out[n++] = (uint8_t)(0x60 | (ssid << 1) | (a == ncalls - 1 ? 1 : 0));
    }
    out[n++] = 0x03;
    out[n++] = 0xF0;
    for (const char *p = info; *p; p++)
        out[n++] = (uint8_t)*p;
    const uint16_t f = fcs(out, n);
    out[n++] = (uint8_t)(f & 0xFF);
    out[n++] = (uint8_t)(f >> 8);
    return n;
}

}    // namespace

void aprs_selftest()
{
    static const char *const CALLS[] = { "APRS", "CT1ABC-9", "WIDE1-1" };
    static const char *const INFO = "!3842.50N/00909.00W>Teste APRS 1200";
    static const char *const EXPECT = "CT1ABC-9>APRS via WIDE1-1";
    static const char *const EXPECT_POS = "38.7083N 9.1500W";
    uint8_t fr[128];
    const int flen = build_frame(fr, CALLS, 3, INFO);

    // Bits: flags, the stuffed frame (LSB first), flags.
    EXT_RAM_BSS_ATTR static uint8_t bits[4096];
    int nb = 0;
    auto flag = [&]() { for (int b = 0; b < 8; b++) bits[nb++] = (0x7E >> b) & 1; };
    for (int k = 0; k < 40; k++)
        flag();
    int run = 0;
    for (int i = 0; i < flen; i++) {
        for (int b = 0; b < 8; b++) {
            const int bit = (fr[i] >> b) & 1;
            bits[nb++] = (uint8_t)bit;
            run = bit ? run + 1 : 0;
            if (run == 5) {
                bits[nb++] = 0;
                run = 0;
            }
        }
    }
    for (int k = 0; k < 4; k++)
        flag();

    struct Case { const char *name; float space_gain, noise, rate; };
    static const Case CASES[] = {
        { "limpo", 1.0f, 0.0f, 1.0f },
        { "de-enfase -6 dB", 0.5f, 0.0f, 1.0f },
        // Noise: sum of 4 uniforms x this (sigma = 1.15 x), white up to 6 kHz.
        // 0.05: Eb/N0 ~18 dB on mark; 0.08: ~14 dB; 0.11: ~11 dB.
        // With the -6 dB tilt the space tone is the weak one: at 0.06 an ideal
        // demodulator gets ~70% of the frames, so these are pass/fail checks
        // with margin, not limits.
        { "de-enfase + ruido 0.05", 0.5f, 0.05f, 1.0f },
        { "ruido 0.05 + taxa +1%", 0.7f, 0.05f, 1.01f },
        { "plano + ruido 0.08", 1.0f, 0.08f, 1.0f },
    };
    EXT_RAM_BSS_ATTR static float block[FFT_SIZE];
    for (const Case &c : CASES) {
        const uint32_t before = aprs_frames();
        char got[256] = "";
        size_t got_len = 0;
        uint32_t rnd = 7;
        float phase = 0.0f, t_bit = 0.0f;
        bool tone_mark = true;
        int pos = 0, bi = 0;
        const float bit_len = FS / (BAUD * c.rate);
        // NRZI: a 0 changes the tone, a 1 keeps it.
        if (bits[0] == 0)
            tone_mark = !tone_mark;
        for (int smp = 0; bi < nb; smp++) {
            const float f = tone_mark ? MARK_HZ : SPACE_HZ;
            phase += 2.0f * (float)M_PI * f / FS;
            if (phase > 2.0f * (float)M_PI)
                phase -= 2.0f * (float)M_PI;
            float noise = 0.0f;
            for (int j = 0; j < 4; j++) {
                rnd = rnd * 1664525u + 1013904223u;
                noise += (float)(int32_t)rnd / 2147483648.0f;
            }
            block[pos++] = 0.3f * (tone_mark ? 1.0f : c.space_gain) * sinf(phase) + c.noise * noise;
            if (++t_bit >= bit_len) {
                t_bit -= bit_len;
                if (++bi < nb && bits[bi] == 0)
                    tone_mark = !tone_mark;
            }
            if (pos == FFT_SIZE) {
                aprs_process(block, FFT_SIZE);
                pos = 0;
                char tmp[APRS_TEXT_MAX + 1];
                const size_t k = aprs_take_text(tmp, sizeof(tmp));
                if (k && got_len + k < sizeof(got)) {
                    memcpy(got + got_len, tmp, k);
                    got_len += k;
                    got[got_len] = 0;
                }
            }
        }
        for (int k = 0; k < FFT_SIZE; k++)
            block[k] = 0.0f;
        aprs_process(block, FFT_SIZE);
        char tmp[APRS_TEXT_MAX + 1];
        const size_t k = aprs_take_text(tmp, sizeof(tmp));
        if (k && got_len + k < sizeof(got)) {
            memcpy(got + got_len, tmp, k);
            got_len += k;
            got[got_len] = 0;
        }
        for (char *p = got; *p; p++)
            if (*p == '\n')
                *p = '|';
        ESP_LOGW("APRS", "autoteste %s: %u trama(s) \"%s\" %s", c.name, (unsigned)(aprs_frames() - before), got,
                 strstr(got, EXPECT) && strstr(got, EXPECT_POS) ? "OK" : "FALHOU");
    }

    // Formatter on frames heard on the air and a few kinds of packet.
    struct Sample { const char *calls[5]; int ncalls; const char *info; };
    static const Sample SAMPLES[] = {
        { { "APZMDM", "CQ0UAI", "CQ0PSI-3", "CQ0PMJ-3", "WIDE2-1" }, 5,
          "=3932.14N/00838.18WrCQ0UAI - Falha de Energia na Rede - Bateria 13.5 Volt" },
        { { "APN382", "CQ0PAL-3", "CQ0PSI-3", "CQ0PMJ-3", "WIDE3-1" }, 5,
          "!3829.43NT00910.11W#DIGIPEATER LAGOA D.ALBUFEIRA(REP)." },
        { { "APJIW4", "CQ0PQC-13", "CQ0PSI-3", "CQ0PMJ-3", "WIDE3-1" }, 5,
          "/071306z3834.80N/00902.75W_000/000g000t073h54j0jDvsQuinta do Conde/A=000098" },
        { { "APRS", "CT1ABC-9", "WIDE1-1" }, 3, ":CT2XYZ   :Ola, teste de mensagem{12" },
        { { "APRS", "CT1ABC-9" }, 2, ">Em QRV na 145.500" },
    };
    EXT_RAM_BSS_ATTR static char out[1024];
    uint8_t f[200];
    for (const Sample &sm : SAMPLES) {
        const int n = build_frame(f, sm.calls, sm.ncalls, sm.info);
        aprs_format(f, n - 2, false, "", out, sizeof(out), nullptr, 0);
        ESP_LOGW("APRS", "formato:\n%s", out);
    }
    {    // SABM (connect request): not APRS, shown raw
        static const char *const CALLS[] = { "CT2XYZ", "CT1ABC" };
        int n = build_frame(f, CALLS, 2, "");
        f[14] = 0x3F;
        n = 15;
        aprs_format(f, n, false, "", out, sizeof(out), nullptr, 0);
        ESP_LOGW("APRS", "formato:\n%s", out);
    }
}

#endif
