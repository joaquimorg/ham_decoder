// Skimmer: PSK31 at 900 Hz, CW at 1500 Hz and a tone keyed by random bits at
// 50 baud (one tone of an RTTY signal) at 2100 Hz, all at once. The PSK and CW
// text must come out on their own lines; the keyed tone must stay quiet.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "skimmer.h"

// PSK31 Varicode of the characters used below.
static const char *varicode(char c)
{
    switch (c) {
    case ' ': return "1";
    case 'C': return "10101101";
    case 'Q': return "111011101";
    case 'D': return "10110101";
    case 'E': return "1110111";
    case 'T': return "1101101";
    case 'A': return "1111101";
    case 'B': return "11101011";
    case 'K': return "101111101";
    case '1': return "10111101";
    case '2': return "11101101";
    case '\n': return "11101";
    default: return "1";
    }
}

static const char *morse(char c)
{
    switch (c) {
    case 'C': return "-.-.";
    case 'Q': return "--.-";
    case 'D': return "-..";
    case 'E': return ".";
    case 'T': return "-";
    case 'S': return "...";
    case 'X': return "-..-";
    case 'Y': return "-.--";
    case 'Z': return "--..";
    case 'K': return "-.-";
    case '2': return "..---";
    default: return "";
    }
}

int main()
{
    const float fs = DSP_SAMPLE_RATE;

    // PSK31 bits.
    std::string pbits(64, '0');
    for (int r = 0; r < 3; r++)
        for (const char *p = "CQ CQ DE CT1ABC CT1ABC K\n"; *p; p++)
            pbits += std::string(varicode(*p)) + "00";
    pbits += std::string(64, '0');
    const int sps = (int)(fs / 31.25f);
    const size_t total = pbits.size() * sps;
    // CW keying at 20 WPM, repeated.
    const int dit = (int)(fs * 1.2f / 20.0f);
    std::string cw;
    while (cw.size() < total) {
        for (const char *p = "CQ CQ DE XYZ XYZ K  "; *p; p++) {
            if (*p == ' ') {
                cw.append(dit * 4, '0');
                continue;
            }
            for (const char *e = morse(*p); *e; e++) {
                cw.append((*e == '-' ? 3 : 1) * dit, '1');
                cw.append(dit, '0');
            }
            cw.append(dit * 2, '0');
        }
    }

    skimmer_init();
    const SkimSignal sig[] = { { 900.0f, -30.0f }, { 1500.0f, -30.0f }, { 2100.0f, -30.0f } };
    static float block[FFT_SIZE];
    uint32_t rnd = 5, bits = 0x5A5A1234;
    float ph_p = 0.0f, ph_c = 0.0f, ph_k = 0.0f, env = 0.0f;
    float sym_r = 1.0f, prev_r = 1.0f;
    int pos = 0, nblocks = 0;
    std::string out;
    for (size_t i = 0; i < total; i++) {
        const size_t s = i / sps, k = i % sps;
        if (k == 0) {
            prev_r = sym_r;
            if (pbits[s] == '0')
                sym_r = -sym_r;
        }
        const float w = 0.5f + 0.5f * cosf((float)M_PI * k / sps);
        const float a_psk = w * prev_r + (1.0f - w) * sym_r;
        ph_p += 2.0f * (float)M_PI * 900.0f / fs;
        ph_c += 2.0f * (float)M_PI * 1500.0f / fs;
        ph_k += 2.0f * (float)M_PI * 2100.0f / fs;
        // Kept small: float phases in the 1e5 range lose the frequency.
        ph_p = fmodf(ph_p, 2.0f * (float)M_PI);
        ph_c = fmodf(ph_c, 2.0f * (float)M_PI);
        ph_k = fmodf(ph_k, 2.0f * (float)M_PI);
        env += ((cw[i] == '1' ? 1.0f : 0.0f) - env) * 0.2f;
        if (i % 240 == 0) {    // 50 baud
            bits = bits * 1103515245u + 12345u;
        }
        const float key = (bits >> 16) & 1 ? 1.0f : 0.0f;
        float n = 0.0f;
        for (int j = 0; j < 4; j++) {
            rnd = rnd * 1664525u + 1013904223u;
            n += (float)(int32_t)rnd / 2147483648.0f;
        }
        block[pos++] = 0.05f * a_psk * cosf(ph_p) + 0.05f * env * sinf(ph_c) + 0.05f * key * sinf(ph_k) +
                       0.02f * n;
        if (pos == FFT_SIZE) {
            skimmer_process(block, pos);
            pos = 0;
            if (++nblocks % 12 == 0) {    // the analyzer reports once a second
                skimmer_update(sig, 3, nullptr, 0);
                char t[SKIM_TEXT_MAX + 1];
                skimmer_take_text(t, sizeof(t));
                out += t;
            }
        }
    }
    for (int r = 0; r < 6; r++) {    // signals gone: the last lines come out
        skimmer_update(nullptr, 0, nullptr, 0);
        char t[SKIM_TEXT_MAX + 1];
        skimmer_take_text(t, sizeof(t));
        out += t;
    }
    printf("%s", out.c_str());
    const bool psk = out.find("PSK31  CQ CQ DE CT1ABC CT1ABC K") != std::string::npos;
    const bool cwok = out.find("CQ DE XYZ XYZ K") != std::string::npos;
    const bool quiet = out.find("2100 Hz") == std::string::npos;
    printf("PSK31 %s, CW %s, tom manipulado %s\n", psk ? "OK" : "FALHOU", cwok ? "OK" : "FALHOU",
           quiet ? "calado OK" : "com texto FALHOU");
    return psk && cwok && quiet ? 0 : 1;
}
