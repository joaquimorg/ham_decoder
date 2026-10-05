// CW decoder on generated Morse: speeds, noise, mistuning (CwReceiver).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "cw_decoder.h"

static const char *morse(char c)
{
    static const char *const LETTERS[26] = {
        ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
        "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--..",
    };
    static const char *const DIGITS[10] = {
        "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----.",
    };
    return c >= 'A' && c <= 'Z' ? LETTERS[c - 'A'] : DIGITS[c - '0'];
}

// Keying (1 = tone) at `wpm`, one entry per sample.
static std::string keying(const char *msg, float wpm, float fs)
{
    const int dit = (int)(fs * 1.2f / wpm);
    std::string k(dit * 20, '0');
    for (const char *p = msg; *p; p++) {
        if (*p == ' ') {
            k.append(dit * 4, '0');    // 3 after the character + 4 = 7
            continue;
        }
        for (const char *e = morse(*p); *e; e++) {
            k.append((*e == '-' ? 3 : 1) * dit, '1');
            k.append(dit, '0');
        }
        k.append(dit * 2, '0');
    }
    k.append(dit * 30, '0');
    return k;
}

int main()
{
    const float fs = DSP_SAMPLE_RATE;
    const char *msg = "CQ CQ DE CT1ABC CT1ABC K TEST 73";
    struct Case { float wpm, noise, offset; };
    const Case cases[] = { { 12, 0.02f, 0 }, { 20, 0.05f, 5 }, { 30, 0.05f, 0 }, { 25, 0.1f, 8 }, { 40, 0.03f, 0 } };
    int fails = 0;
    for (const Case &c : cases) {
        CwReceiver rx;
        rx.set_tone(700.0f);
        const std::string k = keying(msg, c.wpm, fs);
        static float block[FFT_SIZE];
        std::string got;
        uint32_t rnd = 3;
        float ph = 0.0f, env = 0.0f;
        int pos = 0;
        for (size_t i = 0; i < k.size(); i++) {
            env += ((k[i] == '1' ? 1.0f : 0.0f) - env) * 0.2f;    // ~0.4 ms edges
            ph += 2.0f * (float)M_PI * (700.0f + c.offset) / fs;
            if (ph > 2.0f * (float)M_PI)
                ph -= 2.0f * (float)M_PI;
            float n = 0.0f;
            for (int j = 0; j < 4; j++) {
                rnd = rnd * 1664525u + 1013904223u;
                n += (float)(int32_t)rnd / 2147483648.0f;
            }
            block[pos++] = 0.1f * env * sinf(ph) + c.noise * n;
            if (pos == FFT_SIZE) {
                rx.process(block, pos);
                pos = 0;
                char t[CW_TEXT_MAX + 1];
                rx.take_text(t, sizeof(t));
                got += t;
            }
        }
        // The first word may go while the speed is learnt (it starts at 20 WPM).
        const bool ok = got.find("CT1ABC CT1ABC K TEST 73") != std::string::npos;
        fails += !ok;
        printf("CW %2.0f wpm ruido %.2f desvio %2.0f Hz: %4.1f wpm \"%s\" %s\n", c.wpm, c.noise, c.offset, rx.wpm(),
               got.c_str(), ok ? "OK" : "FALHOU");
    }
    return fails ? 1 : 0;
}
