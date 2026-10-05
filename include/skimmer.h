#pragma once

#include <stddef.h>

#include "config.h"

// Multi-channel CW / PSK31 decoding ("skimmer"): besides the main decoders,
// which follow the strongest signal, up to SKIM_CHANNELS other narrow signals
// in the spectrum get a CW and a BPSK31 receiver each. A channel shows the
// PSK31 text when its PSK squelch opens and the CW text otherwise (CW lines
// that look like noise - mostly E, T, I - are dropped). Text comes out a line
// at a time, prefixed with the frequency and the mode.

struct SkimSignal {
    float hz;    // centre
    float db;    // level (for the order)
};

void skimmer_init();
void skimmer_set_enabled(bool on);
bool skimmer_enabled();

// Once per analysis report: the narrow signals seen (strongest first) and the
// frequencies the main decoders are on (left to them).
void skimmer_update(const SkimSignal *sig, int n, const float *busy_hz, int nbusy);

// DSP-rate audio, every block.
void skimmer_process(const float *x, int n);

// Copies the lines finished since the last call (NUL-terminated) and clears them.
size_t skimmer_take_text(char *buf, size_t size);

// Channels in use and the frequency of channel i (0 = free), for markers.
int skimmer_channels();
float skimmer_channel_hz(int i);
