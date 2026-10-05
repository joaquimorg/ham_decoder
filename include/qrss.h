#pragma once

#include "config.h"

// QRSS / DFCW grabber: a slow, narrow waterfall for very slow CW beacons
// (QRSS3, QRSS10, DFCW). The audio around a centre frequency is mixed to
// baseband, decimated to QRSS_RATE and analysed with a QRSS_FFT-point FFT
// (QRSS_RATE / QRSS_FFT = 0.37 Hz per bin), half-overlapped: one column of
// UI_QRSS_BINS levels (the middle of the band, lowest frequency first) every
// QRSS_FFT / 2 / QRSS_RATE = 1.37 s goes to ui_hub. Nothing is decoded: the
// slow Morse is read by eye.

void qrss_init();
void qrss_set_center(float hz);    // audio frequency at the middle of the view
float qrss_center();
float qrss_bin_hz();               // frequency step between the levels of a column

// DSP-rate audio, every block.
void qrss_process(const float *x, int n);
