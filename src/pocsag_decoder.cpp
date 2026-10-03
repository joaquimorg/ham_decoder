#include "pocsag_decoder.h"

#if POCSAG_ENABLE

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_timer.h"

namespace {

constexpr float FS = (float)DSP_SAMPLE_RATE;
constexpr int NRATES = 3;
const int RATES[NRATES] = { 512, 1200, 2400 };

constexpr uint32_t SYNC = 0x7CD215D8;
constexpr uint32_t IDLE = 0x7A89C197;
constexpr int SYNC_MAX_ERRORS = 2;     // bits off in the sync codeword while hunting
constexpr int RESYNC_MAX_ERRORS = 4;   // ... when one is expected after a batch

// Bit clock: phase in bits since the last sample; transitions belong at 0.5.
constexpr float PLL_GAIN = 0.2f;

constexpr int MSG_MAX_BITS = 80 * 20;  // 80 message codewords
constexpr int LINE_MAX = 200;

// BCH(31,21): generator x^10 + x^9 + x^8 + x^6 + x^5 + x^3 + 1.
constexpr uint32_t BCH_POLY = 0x769;

uint32_t bch_syndrome(uint32_t cw)
{
    uint32_t r = cw >> 1;    // bits 31..1 (bit 0 is the even parity)
    for (int i = 30; i >= 10; i--) {
        if (r & (1u << i))
            r ^= BCH_POLY << (i - 10);
    }
    return r & 0x3FF;
}

[[maybe_unused]] bool even_parity(uint32_t cw) { return (__builtin_popcount(cw) & 1) == 0; }

// Corrects up to two bit errors. Returns the number corrected, -1 if the
// codeword cannot be repaired. Two-bit correction makes almost half of all
// random words "valid", so the callers weigh it (see Demod::end_message).
int bch_correct(uint32_t &cw)
{
    if (bch_syndrome(cw) == 0)
        return 0;    // a wrong parity bit alone does not matter
    for (int i = 1; i < 32; i++) {
        const uint32_t t = cw ^ (1u << i);
        if (bch_syndrome(t) == 0) {
            cw = t;
            return 1;
        }
    }
    for (int i = 1; i < 32; i++) {
        for (int j = i + 1; j < 32; j++) {
            const uint32_t t = cw ^ (1u << i) ^ (1u << j);
            if (bch_syndrome(t) == 0) {
                cw = t;
                return 2;
            }
        }
    }
    return -1;
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


EXT_RAM_BSS_ATTR char text[POCSAG_TEXT_MAX];
size_t text_len = 0;
uint32_t messages = 0;
int last_baud = 0;
int64_t last_us = 0;

void append(const char *s)
{
    while (*s && text_len < sizeof(text))
        text[text_len++] = *s++;
}

struct Demod {
    int baud = 0;
    float spb = 0.0f;         // samples per bit
    float step = 0.0f;        // 1 / spb
    int box = 1;              // low-pass: mean of this many samples
    float ring[32] = {};
    float box_sum = 0.0f;
    int box_pos = 0;
    float phase = 0.0f;
    bool prev_level = false;

    uint32_t sr = 0;          // last 32 bits
    enum { HUNT, BATCH, RESYNC } state = HUNT;
    bool inverted = false;
    int bit_count = 0;        // bits of the current codeword
    int cw_index = 0;         // codeword index in the batch (0..15)

    // Message being assembled.
    bool in_msg = false;
    uint32_t ric = 0;
    int func = 0;
    uint8_t bits[MSG_MAX_BITS / 8];
    int nbits = 0;
    int addr_err = 0;         // bits corrected in the address codeword
    int ncw = 0, nbad = 0;    // message codewords, of which 2-bit corrected or lost

    void init(int b)
    {
        baud = b;
        spb = FS / b;
        step = 1.0f / spb;
        box = (int)(spb * 0.5f + 0.5f);
        if (box < 1)
            box = 1;
        if (box > 32)
            box = 32;
    }

    void put_bit(int v)
    {
        if (nbits < MSG_MAX_BITS) {
            if (v)
                bits[nbits >> 3] |= (uint8_t)(0x80 >> (nbits & 7));
            else
                bits[nbits >> 3] &= (uint8_t)~(0x80 >> (nbits & 7));
            nbits++;
        }
    }
    int get_bit(int i) const { return (bits[i >> 3] >> (7 - (i & 7))) & 1; }

    void end_message()
    {
        if (!in_msg)
            return;
        in_msg = false;
        // A false sync (noise) yields random codewords, about half of which
        // pass a 2-bit correction: keep only messages whose address needed at
        // most one bit and most of whose text is clean; a message without
        // text (tone only) needs a perfect address.
        if (addr_err > 1 || nbad * 3 > ncw || (ncw == 0 && addr_err != 0))
            return;
        char line[LINE_MAX];
        int k = utc_stamp(line, sizeof(line));
        k += snprintf(line + k, sizeof(line) - k, "RIC %07u F%d %d%s: ", (unsigned)ric, func, baud,
                      nbad ? " (?)" : "");
        // Function 0 is numeric by convention; the others carry 7-bit text.
        if (func == 0) {
            static const char DIGITS[] = "0123456789*U -)(";
            for (int i = 0; i + 4 <= nbits && k < LINE_MAX - 2; i += 4) {
                const int d = get_bit(i) | get_bit(i + 1) << 1 | get_bit(i + 2) << 2 | get_bit(i + 3) << 3;
                line[k++] = DIGITS[d];
            }
            while (k > 0 && line[k - 1] == ' ')
                k--;    // padding
        } else {
            for (int i = 0; i + 7 <= nbits && k < LINE_MAX - 2; i += 7) {
                int c = 0;
                for (int b = 0; b < 7; b++)
                    c |= get_bit(i + b) << b;
                if (c >= 0x20 && c < 0x7F)
                    line[k++] = (char)c;
                else if (c == '\n' || c == '\r')
                    line[k++] = ' ';
            }
        }
        line[k++] = '\n';
        line[k] = 0;
        append(line);
        messages++;
        last_baud = baud;
        last_us = esp_timer_get_time();
    }

    void codeword(uint32_t cw)
    {
        if (cw == IDLE) {
            end_message();
            return;
        }
        const int err = bch_correct(cw);
        if (err < 0) {
            if (in_msg) {    // keep going: the rest may still be readable
                ncw++;
                nbad++;
            }
            return;
        }
        if (cw == IDLE) {
            end_message();
            return;
        }
        if (!(cw & 0x80000000u)) {    // address codeword
            end_message();
            in_msg = true;
            ric = ((cw >> 13) & 0x3FFFF) << 3 | (uint32_t)(cw_index >> 1);
            func = (int)((cw >> 11) & 3);
            nbits = 0;
            addr_err = err;
            ncw = nbad = 0;
        } else if (in_msg) {          // message codeword: 20 data bits
            ncw++;
            if (err > 1)
                nbad++;
            for (int b = 30; b >= 11; b--)
                put_bit((cw >> b) & 1);
        }
    }

    void bit(int b)
    {
        sr = (sr << 1) | (uint32_t)b;
        switch (state) {
        case HUNT: {
            const int e = __builtin_popcount(sr ^ SYNC), ei = __builtin_popcount(sr ^ ~SYNC);
            if (e <= SYNC_MAX_ERRORS || ei <= SYNC_MAX_ERRORS) {
                inverted = ei < e;
                state = BATCH;
                bit_count = 0;
                cw_index = 0;
            }
            break;
        }
        case BATCH:
            if (++bit_count == 32) {
                bit_count = 0;
                codeword(inverted ? ~sr : sr);
                if (++cw_index == 16)
                    state = RESYNC;
            }
            break;
        case RESYNC:
            if (++bit_count == 32) {
                bit_count = 0;
                const uint32_t w = inverted ? ~sr : sr;
                if (__builtin_popcount(w ^ SYNC) <= RESYNC_MAX_ERRORS) {
                    state = BATCH;
                    cw_index = 0;
                } else {
                    end_message();
                    state = HUNT;
                }
            }
            break;
        }
    }

    void push(float x)
    {
        box_sum += x - ring[box_pos];
        ring[box_pos] = x;
        if (++box_pos == box)
            box_pos = 0;
        const bool level = box_sum > 0.0f;
        phase += step;
        if (level != prev_level) {
            phase += (0.5f - phase) * PLL_GAIN;    // a transition belongs half-way between samples
            prev_level = level;
        }
        if (phase >= 1.0f) {
            phase -= 1.0f;
            bit(level ? 1 : 0);
        }
    }
};

EXT_RAM_BSS_ATTR Demod demods[NRATES];

}    // namespace

void pocsag_init()
{
    for (int r = 0; r < NRATES; r++)
        demods[r].init(RATES[r]);
}

void pocsag_process(const float *x, int n)
{
    for (int i = 0; i < n; i++) {
        for (int r = 0; r < NRATES; r++)
            demods[r].push(x[i]);
    }
}

size_t pocsag_take_text(char *buf, size_t size)
{
    if (size == 0)
        return 0;
    const size_t n = text_len < size - 1 ? text_len : size - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    text_len = 0;
    return n;
}

uint32_t pocsag_messages() { return messages; }
int pocsag_last_baud() { return last_baud; }
int64_t pocsag_last_us() { return last_us; }

#if POCSAG_SELFTEST

#include "esp_log.h"

namespace {

uint32_t bch_encode(uint32_t data21)    // data in bits 31..11 of the result
{
    uint32_t cw = data21 << 11;
    cw |= bch_syndrome(cw) << 1;
    if (!even_parity(cw))
        cw |= 1;
    return cw;
}

// Codewords of one transmission: RIC with a 7-bit text (func 3) or digits (func 0).
int build(uint32_t *cw, uint32_t ric, int func, const char *msg)
{
    int n = 0;
    const int frame = ric & 7;
    uint32_t data[64];
    int nd = 0;
    if (func == 0) {
        static const char DIGITS[] = "0123456789*U -)(";
        uint32_t acc = 0;
        int nb = 0;
        auto put = [&](int d) {
            for (int b = 0; b < 4; b++) {
                acc = (acc << 1) | ((d >> b) & 1);
                if (++nb == 20) {
                    data[nd++] = acc;
                    acc = 0;
                    nb = 0;
                }
            }
        };
        for (const char *p = msg; *p; p++) {
            const char *q = strchr(DIGITS, *p);
            put(q ? (int)(q - DIGITS) : 12);
        }
        while (nb)
            put(12);    // spaces pad the last codeword
    } else {
        uint32_t acc = 0;
        int nb = 0;
        for (const char *p = msg; *p || nb; p++) {
            const int c = *p ? *p : 0;
            for (int b = 0; b < 7; b++) {
                acc = (acc << 1) | ((c >> b) & 1);
                if (++nb == 20) {
                    data[nd++] = acc;
                    acc = 0;
                    nb = 0;
                }
            }
            if (!*p) {
                if (nb) {
                    acc <<= 20 - nb;
                    data[nd++] = acc;
                    nb = 0;
                }
                break;
            }
        }
    }
    // Batch(es): sync + 16 codewords, the address in its frame, idle elsewhere.
    int pos = 0;    // codeword position in the batch
    cw[n++] = SYNC;
    for (; pos < frame * 2; pos++)
        cw[n++] = IDLE;
    cw[n++] = bch_encode((ric >> 3) << 2 | (uint32_t)func);
    pos++;
    for (int i = 0; i < nd; i++) {
        if (pos == 16) {
            cw[n++] = SYNC;
            pos = 0;
        }
        cw[n++] = bch_encode(1u << 20 | data[i]);
        pos++;
    }
    for (bool tail = true; pos < 16 || tail; pos++) {
        if (pos == 16) {
            cw[n++] = SYNC;
            pos = 0;
            tail = false;
        }
        cw[n++] = IDLE;
    }
    return n;
}

}    // namespace

void pocsag_selftest()
{
    struct Case { const char *name; int baud; bool invert; int errors; float hp_hz, noise; };
    static const Case CASES[] = {
        { "512", 512, false, 0, 0.0f, 0.0f },
        { "1200 invertido", 1200, true, 0, 0.0f, 0.0f },
        { "2400", 2400, false, 0, 0.0f, 0.0f },
        // 1 bit wrong in the address, 2 in a text codeword (both corrected).
        { "1200 bits errados", 1200, false, 2, 0.0f, 0.0f },
        // AC coupling at 30 Hz: a discriminator output through a coupling
        // capacitor. The audio of a speaker output (cut around 100-300 Hz)
        // is beyond this NRZ slicer at 512 and 1200 baud.
        { "1200 filtro 30 Hz + ruido", 1200, false, 0, 30.0f, 0.15f },
        { "512 filtro 30 Hz + ruido", 512, true, 0, 30.0f, 0.15f },
        { "so ruido (sem mensagens)", 0, false, 0, 0.0f, 0.3f },
    };
    EXT_RAM_BSS_ATTR static uint32_t cw[200];
    EXT_RAM_BSS_ATTR static float block[FFT_SIZE];
    for (const Case &c : CASES) {
        if (c.baud == 0) {    // noise only: nothing may come out
            const uint32_t before = pocsag_messages();
            uint32_t rnd = 11;
            for (int blk = 0; blk < 20 * DSP_SAMPLE_RATE / FFT_SIZE; blk++) {
                for (int i = 0; i < FFT_SIZE; i++) {
                    float noise = 0.0f;
                    for (int j = 0; j < 4; j++) {
                        rnd = rnd * 1664525u + 1013904223u;
                        noise += (float)(int32_t)rnd / 2147483648.0f;
                    }
                    block[i] = c.noise * noise;
                }
                pocsag_process(block, FFT_SIZE);
            }
            char tmp[POCSAG_TEXT_MAX + 1];
            pocsag_take_text(tmp, sizeof(tmp));
            const unsigned n = (unsigned)(pocsag_messages() - before);
            ESP_LOGW("POCSAG", "autoteste %s: %u msg em 20 s %s", c.name, n, n == 0 ? "OK" : "FALHOU");
            continue;
        }
        const uint32_t ric = 1234567;
        const bool alpha = c.baud != 2400;
        const int ncw = build(cw, ric, alpha ? 3 : 0, alpha ? "TESTE POCSAG CT1ABC" : "912345678");
        const char *expect = alpha ? "RIC 1234567 F3" : "RIC 1234567 F0";
        const char *expect_txt = alpha ? "TESTE POCSAG CT1ABC" : "912345678";
        // Bit errors: one in the address codeword (index 1 + frame * 2 after
        // the sync), two in the codeword after it (text).
        if (c.errors) {
            const int idx = 1 + (int)(ric & 7) * 2;
            cw[idx] ^= 0x00100000u;
            cw[idx + 1] ^= 0x00040000u | 0x00000800u;
        }
        const uint32_t before = pocsag_messages();
        char got[512] = "";
        size_t got_len = 0;
        uint32_t rnd = 3;
        float hp_y = 0.0f, hp_x = 0.0f, lp = 0.0f;
        const float hp_a = c.hp_hz > 0.0f ? 1.0f / (1.0f + 2.0f * (float)M_PI * c.hp_hz / FS) : 1.0f;
        const float lp_a = 1.0f - expf(-2.0f * (float)M_PI * 3000.0f / FS);    // receiver audio bandwidth
        const int pre = 576;
        const int total_bits = pre + ncw * 32 + 64;
        const float spb = FS / c.baud;
        int pos = 0;
        for (int smp = 0; smp < (int)(total_bits * spb); smp++) {
            const int bi = (int)(smp / spb);
            int b;
            if (bi < pre)
                b = bi & 1 ? 0 : 1;    // preamble 1010...
            else if (bi < pre + ncw * 32)
                b = (cw[(bi - pre) / 32] >> (31 - (bi - pre) % 32)) & 1;
            else
                b = 1;
            if (c.invert)
                b ^= 1;
            float v = b ? 0.3f : -0.3f;
            lp += lp_a * (v - lp);
            v = lp;
            if (c.hp_hz > 0.0f) {    // AC coupling of the receiver's audio
                hp_y = hp_a * (hp_y + v - hp_x);
                hp_x = v;
                v = hp_y;
            }
            float noise = 0.0f;
            for (int j = 0; j < 4; j++) {
                rnd = rnd * 1664525u + 1013904223u;
                noise += (float)(int32_t)rnd / 2147483648.0f;
            }
            block[pos++] = v + c.noise * noise;
            if (pos == FFT_SIZE) {
                pocsag_process(block, FFT_SIZE);
                pos = 0;
                char tmp[POCSAG_TEXT_MAX + 1];
                const size_t k = pocsag_take_text(tmp, sizeof(tmp));
                if (k && got_len + k < sizeof(got)) {
                    memcpy(got + got_len, tmp, k);
                    got_len += k;
                    got[got_len] = 0;
                }
            }
        }
        for (char *p = got; *p; p++)
            if (*p == '\n')
                *p = '|';
        const bool ok = strstr(got, expect) && strstr(got, expect_txt);
        ESP_LOGW("POCSAG", "autoteste %s: %u msg \"%s\" %s", c.name, (unsigned)(pocsag_messages() - before), got,
                 ok ? "OK" : "FALHOU");
    }
}

#endif    // POCSAG_SELFTEST

#endif    // POCSAG_ENABLE
