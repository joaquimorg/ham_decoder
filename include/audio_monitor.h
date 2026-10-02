#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

// Monitor: the audio the analyzer receives, played on the board's speaker
// through the ES8311 DAC (AUDIO_MONITOR, FNK0104S). Works with any audio
// source. Without AUDIO_MONITOR the calls do nothing.

#if AUDIO_MONITOR
// After audio_source_init(). Starts the output task.
void audio_monitor_init();

// Queues mono samples (24-bit range, AUDIO_SAMPLE_RATE). Never blocks: what
// does not fit is dropped.
void audio_monitor_write(const int32_t *samples, size_t n);

// 0..100 %; 0 = off (DAC muted, amplifier disabled). May be called before
// audio_monitor_init(). Does nothing if the volume did not change.
void audio_monitor_set_volume(int percent);
#else
static inline void audio_monitor_init() {}
static inline void audio_monitor_write(const int32_t *, size_t) {}
static inline void audio_monitor_set_volume(int) {}
#endif
