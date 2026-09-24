#pragma once

// HF FAX (WEFAX) decoder: FM 1500 Hz (black) .. 2300 Hz (white). An image
// starts on the APT start tone (300 Hz for IOC 576, 675 Hz for IOC 288), is
// aligned on the phasing lines and ends on the 450 Hz stop tone; it can also
// be started and stopped by hand. Lines go to the web page (web_image_*).
// Mirrored and validated off-target in tools/fax_sim.py.

enum FaxState { FAX_IDLE = 0, FAX_PHASING = 1, FAX_RECEIVING = 2 };

void fax_init();

void fax_set_lpm(int lpm);        // 60, 90, 120 or 240 lines per minute
void fax_set_ioc(int ioc);        // 576 or 288
void fax_set_auto(bool on);       // start on the APT tone

// Requests from other tasks, applied on the next fax_process().
void fax_request_start();         // start receiving now, without phasing
void fax_request_stop();

FaxState fax_state();
int fax_lines();                  // lines of the current image

// hz: instantaneous frequency from fm_demod_process().
void fax_process(const float *hz, int n);
