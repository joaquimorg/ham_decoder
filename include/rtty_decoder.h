#pragma once

#include <stddef.h>

// RTTY (Baudot/ITA2, 1 start + 5 data + 1.5 stop bits) decoder. Runs on the
// DSP-rate stream (DSP_SAMPLE_RATE, float) on the two FSK tones the analyzer
// finds. Mirrored and validated off-target in tools/rtty_sim.py.

enum RttyPolarity { RTTY_POL_AUTO = 0, RTTY_POL_NORMAL = 1, RTTY_POL_REVERSE = 2 };

void rtty_init();

// Tones to follow (any order); 0 unlocks and resets the decoder.
void rtty_set_tones(float f1, float f2);
void rtty_set_baud(float baud);
// NORMAL: mark = lower tone (amateur convention); AUTO: the tone that dominates
// over time is the mark (RTTY idles on mark).
void rtty_set_polarity(RttyPolarity pol);

float rtty_mark_hz();     // 0 = unlocked
float rtty_space_hz();
float rtty_baud();
bool rtty_active();       // FSK present and decoding well-formed characters

void rtty_process(const float *x, int n);

// Copies the text decoded since the last call (NUL-terminated) and clears it.
size_t rtty_take_text(char *buf, size_t size);
