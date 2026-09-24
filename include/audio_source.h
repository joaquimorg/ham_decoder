#pragma once

#include <stddef.h>
#include <stdint.h>

// Audio input, selected by AUDIO_SOURCE in config.h:
//   AUDIO_SRC_PCM1808 -> src/pcm1808_source.cpp (I2S, 24-bit)
//   AUDIO_SRC_ADC     -> src/adc_source.cpp     (internal ADC1, 12-bit)
// Both deliver mono samples at AUDIO_SAMPLE_RATE, DC-free, with full scale
// mapped to +-AUDIO_FULL_SCALE.

void audio_source_init();

// Blocks until samples are available; writes up to `max` samples and
// returns how many were written.
size_t audio_source_read(int32_t *out, size_t max);

// Prints pending source diagnostics. Called from the analysis task so the
// capture task never blocks on the console.
void audio_source_log();
