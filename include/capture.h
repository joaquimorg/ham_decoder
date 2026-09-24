#pragma once

// Records what the decoders see (DSP_SAMPLE_RATE, float) into PSRAM and dumps
// it over the console as base64 between CAPTURE_BEGIN / CAPTURE_END lines.
// tools/capture_to_wav.py turns a saved monitor log into a WAV file.
// Enabled with CAPTURE_SECONDS > 0 in config.h; runs once per boot.

void capture_init();

// Feeds one DSP block; `start` arms the recording (e.g. when CW locks).
// Blocks while dumping once the buffer is full.
void capture_push(const float *x, int n, bool start);
