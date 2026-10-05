// Olivia / Contestia: generated transmissions (encoder written from the
// protocol, as fldigi sends it) through the decoder, with noise and mistuning.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "mfsk_decoder.h"

static void ifht(float *d, int n)
{
    for (int step = n / 2; step; step /= 2) {
        for (int p = 0; p < n; p += 2 * step) {
            for (int q = p; q < p + step; q++) {
                const float a = d[q], b = d[q + step];
                d[q] = a - b;
                d[q + step] = a + b;
            }
        }
    }
}

// Symbols (tone numbers before the Gray code) of one block of bps characters.
static void encode_block(const uint8_t *chars, int bps, bool contestia, uint8_t *out)
{
    const int spb = contestia ? 32 : 64, shift = contestia ? 5 : 13;
    const uint64_t code = contestia ? 0xEDB88320ULL : 0xE257E6D0291574ECULL;
    memset(out, 0, spb);
    for (int fb = 0; fb < bps; fb++) {
        uint8_t c = chars[fb];
        if (contestia) {
            if (c >= 'a' && c <= 'z')
                c += 'A' - 'a';
            if (c == ' ')
                c = 59;
            else if (c == '\r')
                c = 60;
            else if (c == '\n')
                c = 0;
            else if (c >= 33 && c <= 90)
                c -= 32;
            else if (c != 0)
                c = '?' - 32;
        } else {
            c &= 2 * spb - 1;
        }
        float buf[64] = {};
        if (c < spb)
            buf[c] = 1.0f;
        else
            buf[c - spb] = -1.0f;
        ifht(buf, spb);
        int bit = (fb * shift) & (spb - 1);
        for (int t = 0; t < spb; t++) {
            if ((code >> bit) & 1)
                buf[t] = -buf[t];
            bit = (bit + 1) & (spb - 1);
        }
        for (int t = 0, rot = 0; t < spb; t++) {
            if (buf[t] < 0.0f)
                out[t] |= (uint8_t)(1 << ((fb + rot) % bps));
            rot = (rot + 1) % bps;
        }
    }
}

static std::string run(bool contestia, int tones, int bw, float noise, float mistune, const char *msg)
{
    int bps = 0;
    while ((1 << bps) < tones)
        bps++;
    const int spb = contestia ? 32 : 64;
    // Blocks: idle, the message (padded), idle.
    std::vector<uint8_t> chars(bps * 2, 0);
    for (const char *p = msg; *p; p++)
        chars.push_back((uint8_t)*p);
    while (chars.size() % bps)
        chars.push_back(0);
    chars.insert(chars.end(), bps * 6, 0);
    std::vector<uint8_t> syms;
    for (size_t i = 0; i < chars.size(); i += bps) {
        uint8_t blk[64];
        encode_block(&chars[i], bps, contestia, blk);
        syms.insert(syms.end(), blk, blk + spb);
    }
    // Hann-shaped tone bursts two symbols long, half overlapped, a random
    // quarter-turn phase step between symbols (as fldigi).
    const float fs = DSP_SAMPLE_RATE, centre = 1500.0f;
    const float spacing = (float)bw / tones;
    const int len = (int)(fs / spacing) * 2, step = len / 2;
    std::vector<float> audio((syms.size() + 2) * step + len, 0.0f);
    uint32_t rnd = 11;
    double ph = 0.0;
    for (size_t s = 0; s < syms.size(); s++) {
        const int tone = syms[s] ^ (syms[s] >> 1);    // Gray
        const double f = centre + mistune - bw / 2.0 + spacing / 2.0 + tone * spacing;
        rnd = rnd * 1664525u + 1013904223u;
        ph += (rnd >> 31) ? M_PI / 2 : -M_PI / 2;
        for (int k = 0; k < len; k++) {
            const double w = 0.5 - 0.5 * cos(2 * M_PI * k / len);
            audio[s * step + k] += (float)(0.1 * w * cos(ph + 2 * M_PI * f * k / fs));
        }
        ph = fmod(ph + 2 * M_PI * f * step / fs, 2 * M_PI);
    }
    mfsk_configure(MFSK_OFF, 0, 0, 0.0f);
    mfsk_configure(contestia ? MFSK_CONTESTIA : MFSK_OLIVIA, tones, bw, centre);
    std::string got;
    float block[FFT_SIZE];
    for (size_t i = 0; i < audio.size(); i += FFT_SIZE) {
        for (int k = 0; k < FFT_SIZE; k++) {
            float n = 0.0f;
            for (int j = 0; j < 4; j++) {
                rnd = rnd * 1664525u + 1013904223u;
                n += (float)(int32_t)rnd / 2147483648.0f;
            }
            block[k] = (i + k < audio.size() ? audio[i + k] : 0.0f) + noise * n;
        }
        mfsk_process(block, FFT_SIZE);
        char t[MFSK_TEXT_MAX + 1];
        mfsk_take_text(t, sizeof(t));
        got += t;
    }
    return got;
}

int main()
{
    mfsk_init();
    struct Case { bool contestia; int tones, bw; float noise, mistune; };
    const Case cases[] = {
        { false, 32, 1000, 0.05f, 0.0f }, { false, 32, 1000, 0.2f, 40.0f }, { false, 8, 250, 0.2f, 10.0f },
        { false, 16, 500, 0.3f, -15.0f }, { false, 32, 1000, 0.35f, 0.0f }, { false, 4, 125, 0.2f, 0.0f }, { false, 64, 2000, 0.1f, 0.0f },
        { true, 32, 1000, 0.1f, 20.0f }, { true, 8, 250, 0.2f, 0.0f }, { true, 4, 250, 0.1f, 0.0f },
    };
    int fails = 0;
    for (const Case &c : cases) {
        const char *msg = c.contestia ? "CQ CQ DE CT1ABC CT1ABC K\n" : "CQ CQ de CT1ABC CT1ABC pse k\n";
        std::string got = run(c.contestia, c.tones, c.bw, c.noise, c.mistune, msg);
        const std::string want(msg, strlen(msg) - 1);
        const bool ok = got.find(want) != std::string::npos;
        fails += !ok;
        for (char &ch : got)
            if (ch == '\n')
                ch = '|';
        printf("%s %d/%d ruido %.2f desvio %+3.0f Hz: snr %.1f desvio %+.1f Hz \"%s\" %s\n",
               c.contestia ? "CONTESTIA" : "OLIVIA", c.tones, c.bw, c.noise, c.mistune, mfsk_snr(), mfsk_offset_hz(),
               got.c_str(), ok ? "OK" : "FALHOU");
    }
    // Noise only, a minute per mode: nothing may print.
    for (int ct = 0; ct < 2; ct++) {
        mfsk_configure(MFSK_OFF, 0, 0, 0.0f);
        mfsk_configure(ct ? MFSK_CONTESTIA : MFSK_OLIVIA, 32, 1000, 1500.0f);
        std::string got;
        uint32_t rnd = 77;
        float block[FFT_SIZE];
        for (int b = 0; b < 700; b++) {
            for (int k = 0; k < FFT_SIZE; k++) {
                float n = 0.0f;
                for (int j = 0; j < 4; j++) {
                    rnd = rnd * 1664525u + 1013904223u;
                    n += (float)(int32_t)rnd / 2147483648.0f;
                }
                block[k] = 0.1f * n;
            }
            mfsk_process(block, FFT_SIZE);
            char t[MFSK_TEXT_MAX + 1];
            mfsk_take_text(t, sizeof(t));
            got += t;
        }
        fails += !got.empty();
        printf("%s so ruido: %zu caracteres \"%s\" %s\n", ct ? "CONTESTIA" : "OLIVIA", got.size(), got.c_str(),
               got.empty() ? "OK" : "FALHOU");
    }
    return fails ? 1 : 0;
}
