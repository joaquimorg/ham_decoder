#pragma once

#include <stdint.h>

// Spectrum analyzer + heuristic signal classifier.
// Consumes blocks of FFT_SIZE samples at DSP_SAMPLE_RATE (float, full scale = 1.0)
// and prints one text-waterfall line per report interval to the console.

void analyzer_init();

// raw_peak: peak absolute sample (full scale = AUDIO_FULL_SCALE) seen by the capture task since the last block.
void analyzer_process_block(const float *x, int32_t raw_peak, uint32_t overruns);
