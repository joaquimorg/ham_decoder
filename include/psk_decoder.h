#pragma once

#include <stddef.h>

#include "config.h"

// PSK31 / PSK63 / PSK125 (BPSK, Varicode) decoder. Runs on the DSP-rate stream
// (DSP_SAMPLE_RATE, float) around the centre frequency the analyzer gives it.
// The three speeds are demodulated in parallel (a symbol-long matched filter
// each) and the one with the cleanest phase decisions is shown; AFC follows
// the carrier within about +-baud/4.

void psk_init();

// Centre frequency to follow; 0 unlocks and resets the decoder.
void psk_set_tone(float hz);

float psk_tone_hz();          // 0 = unlocked
float psk_baud();             // speed being shown (31.25, 62.5, 125)
const char *psk_mode_name();  // "PSK31", "PSK63", "PSK125"
float psk_quality();          // 0..1, mean cos(2 x phase step) of the shown speed
bool psk_active();            // squelch open: clean BPSK with phase reversals

void psk_process(const float *x, int n);

// Copies the text decoded since the last call (NUL-terminated) and clears it.
size_t psk_take_text(char *buf, size_t size);

#if PSK_SELFTEST
// Decodes generated BPSK31/63/125 (noise, mistuning) and logs the result.
void psk_selftest();
#endif
