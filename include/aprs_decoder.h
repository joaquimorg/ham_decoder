#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

// APRS / AX.25 packet at 1200 baud (AFSK Bell 202: mark 1200 Hz, space
// 2200 Hz) from the audio of an FM receiver, and at 300 baud (mark 1600 Hz,
// space 1800 Hz) from an HF SSB receiver (frames tagged "HF"). Runs all the time on the
// DSP-rate stream (DSP_SAMPLE_RATE, float): packets are short bursts, and only
// frames with a valid FCS (CRC-16) come out, so noise prints nothing.
// Each frame is given in the usual monitor format:
//   SOURCE>DEST,DIGI1*,DIGI2:information

void aprs_init();
void aprs_process(const float *x, int n);

// Copies the frames decoded since the last call (one per line,
// NUL-terminated) and clears them.
size_t aprs_take_text(char *buf, size_t size);

uint32_t aprs_frames();             // valid frames since boot
const char *aprs_last_source();     // source call of the last frame ("" = none)
int64_t aprs_last_us();             // esp_timer time of the last frame (0 = none)
bool aprs_last_hf();                // the last frame came on HF (300 baud)

#if APRS_SELFTEST
// Decodes generated AFSK frames (de-emphasis tilt, noise, rate error) and logs the result.
void aprs_selftest();
#endif
