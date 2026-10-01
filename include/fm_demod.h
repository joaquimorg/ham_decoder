#pragma once

// FM discriminator shared by the image modes (FAX, SSTV): instantaneous
// frequency of the DSP-rate stream, band-limited to FM_CENTER_HZ +-
// FM_CUTOFF_HZ. Mirrored in tools/fax_sim.py.

void fm_demod_init();

// hz[i] = instantaneous frequency (Hz) of x[i]; n <= FFT_SIZE. mag[i], if given,
// is the amplitude of the filtered carrier (0 where there is no signal).
void fm_demod_process(const float *x, float *hz, int n, float *mag = nullptr);
