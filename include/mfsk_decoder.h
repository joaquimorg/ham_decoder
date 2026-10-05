#pragma once

#include <stddef.h>

#include "config.h"

// Olivia and Contestia (MFSK with Walsh-Hadamard FEC, as fldigi). The mode,
// the number of tones and the bandwidth are set by the user (the signal does
// not announce them); the decoder finds the timing of the FEC blocks and the
// tuning within +-MFSK_SEARCH_BINS half tone spacings of the centre given.
//
// Each block carries one character per bit of the symbol (5 for 32 tones),
// spread over 64 symbols (Olivia, 7-bit characters) or 32 (Contestia, 6-bit,
// upper case): the audio around the centre is mixed to baseband, decimated to
// 4 kHz and analysed with an FFT twice per symbol; the tone energies give
// soft bits (Gray coded), which are descrambled and decoded with a fast
// Hadamard transform for every block timing and tuning step at once. The best
// one, once its signal-to-noise ratio passes MFSK_SNR_MIN, prints.

enum MfskMode { MFSK_OFF = 0, MFSK_OLIVIA = 1, MFSK_CONTESTIA = 2 };

void mfsk_init();

// tones: 4..64 (power of two); bandwidth: 125, 250, 500, 1000 or 2000 Hz.
// Combinations whose symbol is too long for the decoder fall back to off.
// Resets the decoder (does nothing if the settings are the same).
void mfsk_configure(MfskMode mode, int tones, int bandwidth, float centre_hz);

MfskMode mfsk_mode();
const char *mfsk_name();       // "OLIVIA 32/1000", "" when off
float mfsk_snr();              // of the best block timing and tuning
float mfsk_offset_hz();        // its tuning error
bool mfsk_active();            // printing

void mfsk_process(const float *x, int n);

// Copies the text decoded since the last call (NUL-terminated) and clears it.
size_t mfsk_take_text(char *buf, size_t size);

// Times the decoding task (core 0, low priority) fell behind and skipped
// ahead, losing some text.
#include <stdint.h>
uint32_t mfsk_overruns();

// Valid combination (and the symbol length it gives).
bool mfsk_valid(int tones, int bandwidth);
