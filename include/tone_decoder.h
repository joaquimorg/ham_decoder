#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

// DTMF digits, CTCSS sub-audible tones and DCS codes, from the audio of an FM receiver.
// Runs all the time on the DSP-rate stream (DSP_SAMPLE_RATE, float).
//  - DTMF: Goertzel on the 8 tones in 25 ms blocks; a digit needs one row and
//    one column tone holding most of the block's energy (twist and the other
//    tones checked) in two blocks running. Each sequence is one line.
//  - CTCSS: the 50 standard tones (67.0-254.1 Hz) on a 1 kHz copy of the
//    audio (low-pass at 300 Hz), Goertzel over 1 s windows every 0.5 s (1 Hz
//    resolution: some tones are 2.4 Hz apart); a tone is reported once it holds
//    for two windows.
//  - DCS: the 23-bit Golay word at 134.4 bit/s on the same 1 kHz copy (DPLL
//    clock, both polarities); a code is reported once its word repeats exactly
//    23 bits apart three times. The word is cyclic, so some rotations are other
//    valid codes: the lowest standard code found is reported.
// CTCSS and DCS need audio below 300 Hz: a line or discriminator output (many
// receivers cut it from the speaker).

void tones_init();
void tones_process(const float *x, int n);

// Copies the text since the last call (DTMF sequences, "CTCSS 88.5 Hz" and
// "DCS 023N" lines, NUL-terminated) and clears it.
size_t tones_take_text(char *buf, size_t size);

float ctcss_hz();             // tone present now, 0 = none
int dcs_code();               // DCS code present now (octal digits as a number, e.g. 023 -> 19), -1 = none
bool dcs_inverted();          // its polarity: false = N, true = I (an inverting receiver swaps them)
const char *dtmf_last();      // digits of the sequence in progress or the last one
int64_t dtmf_last_us();       // esp_timer time of the last digit (0 = none)

#if TONES_SELFTEST
// Decodes generated DTMF sequences and CTCSS tones (noise, voice-like tones) and logs the result.
void tones_selftest();
#endif
