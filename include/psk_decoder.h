#pragma once

#include <stddef.h>

#include "config.h"

// PSK (Varicode) decoder: BPSK31/63/125/250/500 and QPSK31/63/125/250/500
// (rate 1/2, K = 5 convolutional code, Viterbi). Runs on the DSP-rate stream
// (DSP_SAMPLE_RATE, float) around the centre frequency it is given. Every
// speed is demodulated in parallel (a symbol-long matched filter each); the
// one with the cleanest phase decisions is shown. BPSK and QPSK are told apart
// by the phase steps (QPSK also steps by 90 degrees) and QPSK is decoded both
// ways round (USB and LSB give opposite phase steps), keeping the one whose
// Viterbi path fits best. AFC follows the carrier within about +-baud/4.

class PskReceiver {
public:
    // all_modes: every speed, BPSK and QPSK (main decoder); false: BPSK31
    // only (the skimmer runs several of these).
    explicit PskReceiver(bool all_modes);

    // Centre frequency to follow; 0 unlocks and resets the decoder.
    void set_tone(float hz);
    float tone_hz() const;          // 0 = unlocked
    float baud() const;             // speed being shown
    const char *mode_name() const;  // "PSK31" .. "QPSK500"
    float quality() const;          // 0..1, phase decision quality of the shown mode
    bool active() const;            // squelch open: clean PSK

    void process(const float *x, int n);

    // Copies the text decoded since the last call (NUL-terminated) and clears it.
    size_t take_text(char *buf, size_t size);

    struct State;

private:
    State *s;
};

// The main decoder (the analyzer tunes it).
void psk_init();
void psk_set_tone(float hz);
float psk_tone_hz();
float psk_baud();
const char *psk_mode_name();
float psk_quality();
bool psk_active();
void psk_process(const float *x, int n);
size_t psk_take_text(char *buf, size_t size);

#if PSK_SELFTEST
// Decodes generated BPSK and QPSK signals (noise, mistuning, LSB) and logs the result.
void psk_selftest();
#endif
