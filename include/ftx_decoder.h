#pragma once

#include "ftx_core.h"

// FT8/FT4 on the board: the analysis task feeds the DSP stream (timestamped
// with the UTC clock, set by NTP or by the web page) to ftx_core; a task on
// core 0 decodes each finished slot and sends the messages to the web page.

void ftx_init();

// Analysis task, once per DSP block (n samples just received).
void ftx_process(const float *x, int n);

// True once the clock holds a plausible UTC time (NTP or the web page).
bool ftx_time_ok();

// Last slot decoded: messages and time taken (ms); -1 before the first.
int ftx_last_count();
int ftx_last_ms();
