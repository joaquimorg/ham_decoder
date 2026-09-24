#pragma once

// SSTV decoder: waits for the VIS header, then decodes the announced mode
// (Martin M1/M2, Scottie S1/S2/DX, Robot 36/72, PD 50/90/120/160/180/240) from
// the FM discriminator output, following the line syncs to correct timing
// and slant. Lines go to the web page (web_image_*). Mirrored and validated
// off-target in tools/sstv_sim.py.

void sstv_init();

void sstv_request_stop();    // from other tasks, applied on the next sstv_process()

bool sstv_receiving();
const char *sstv_mode_name();    // current or last image, "" before the first
int sstv_lines();                // image lines of the current or last image

// hz: instantaneous frequency from fm_demod_process().
void sstv_process(const float *hz, int n);
