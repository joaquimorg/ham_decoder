#pragma once

#include <stdint.h>

#include "config.h"

// Feld-Hell: an on-off keyed tone that "paints" text, 17.5 columns of 14
// pixels per second (245 pixels/s, 7 columns per character). Nothing is
// decoded to text: the tone level is sampled at the pixel rate and handed to
// the interfaces as columns (ui_push_hell_column), which draw each one twice
// (stacked), as Hell receivers do, so a whole copy of the text is always in
// view without any sync. Runs on the DSP-rate stream on the tone set with
// hell_set_tone (the manual CW tone, else the strongest peak).

#define HELL_ROWS 14    // = UI_HELL_ROWS

void hell_init();
void hell_set_tone(float hz);    // 0 = stop
float hell_tone_hz();
void hell_process(const float *x, int n);

#if HELL_SELFTEST
// Sends a known pixel pattern through the decoder and checks the columns.
void hell_selftest();
#endif
