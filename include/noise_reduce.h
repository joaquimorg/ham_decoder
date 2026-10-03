#pragma once

#include "config.h"

// Spectral noise reduction for the speaker monitor, mono, at NR_RATE
// (the speaker band ends at 3.5 kHz; a 48 kHz capture is first decimated). 128-point STFT, 50 % overlap.
// The noise floor is a low percentile of the power across the voice-band bins of each frame (smoothed over time), not a
// per-bin minimum over time, so steady tones (fax, SSTV, RTTY) are kept.
// Bins near the floor are attenuated by up to MONITOR_NR_DEPTH_DB.
// One sample in, one sample out (at NR_RATE), delayed by NR_LATENCY_SAMPLES.

#define NR_RATE             12000
#define NR_DECIM            (AUDIO_SAMPLE_RATE / NR_RATE)    // 1 with the ES8311
#if AUDIO_SAMPLE_RATE % NR_RATE
#error "AUDIO_SAMPLE_RATE must be a multiple of NR_RATE"
#endif
#define NR_LATENCY_SAMPLES  64

#if MONITOR_NR
void nr_init();
float nr_process(float x);
#endif
