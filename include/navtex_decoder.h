#pragma once

#include <stddef.h>

#include "config.h"

// NAVTEX / SITOR-B: 100 baud FSK (170 Hz shift), CCIR 476 code (7 bits, four
// of them marks), every character sent twice - the repetition ("rep") 35 bits
// before the main ("alpha") copy - so a damaged character is replaced by its
// copy. Runs on the DSP-rate stream (DSP_SAMPLE_RATE, float) on the two FSK
// tones the analyzer finds. Polarity and the character alignment (one of 14
// bit offsets) are found from the stream itself.

void navtex_init();

// Tones to follow (any order); 0 unlocks and resets the decoder.
void navtex_set_tones(float f1, float f2);
float navtex_mark_hz();     // 0 = unlocked
float navtex_space_hz();
bool navtex_active();       // in sync, characters valid

void navtex_process(const float *x, int n);

// Copies the text decoded since the last call (NUL-terminated) and clears it.
size_t navtex_take_text(char *buf, size_t size);

#if NAVTEX_SELFTEST
// Decodes a generated transmission (phasing, message, noise, mistuning) and logs the result.
void navtex_selftest();
#endif
