#pragma once

#include <stddef.h>

// Morse (CW) decoder. Runs continuously on the DSP-rate stream (DSP_SAMPLE_RATE,
// float, full scale = 1.0) and decodes the tone the analyzer tells it to follow.

void cw_init();

// Tone to follow, from the spectrum analysis; 0 unlocks and resets the decoder.
void cw_set_tone(float hz);

// Mark/space level ratio needed before decoding (default CW_MIN_CONTRAST).
void cw_set_min_contrast(float ratio);

// Current tone (0 = unlocked) and speed estimate.
float cw_tone_hz();
float cw_wpm();

void cw_process(const float *x, int n);

// Copies the text decoded since the last call (NUL-terminated) and clears it.
// Returns the number of characters copied.
size_t cw_take_text(char *buf, size_t size);

// One line of keying statistics since the last call (for CW_DEBUG).
size_t cw_debug_line(char *buf, size_t size);
