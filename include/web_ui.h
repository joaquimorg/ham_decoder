#pragma once

#include <stdint.h>
#include <stdbool.h>

// Wi-Fi + web interface: live spectrum/waterfall, classification, decoded CW
// text and settings. Joins the Wi-Fi network in g_settings; without it (or if
// it cannot connect) it also opens the "RX-Analyzer" access point at
// http://192.168.4.1 so the network can be configured from the page.

// Spectrum rows sent to the page: bins 0..WEB_BINS-1 of the analyzer FFT
// (DSP_SAMPLE_RATE / FFT_SIZE Hz each), level in 0.5 dB steps from -120 dBFS.
#define WEB_BINS 300

struct WebStatus {
    char label[48];
    float snr_db;
    float rms_dbfs;
    float peak_dbfs;
    float tone_hz;      // CW decoder tone, 0 = unlocked
    float wpm;
    bool clip;
    float rtty_mark_hz;     // 0 = unlocked
    float rtty_space_hz;
    bool rtty_active;       // squelch open
};

enum WebTextChannel { WEB_TEXT_CW = 0, WEB_TEXT_RTTY = 1 };

void web_start();

// Producers (analysis task).
void web_push_spectrum(const uint8_t *row);    // WEB_BINS values, once per FFT frame
void web_push_status(const WebStatus &st);     // once per report
void web_push_text(WebTextChannel ch, const char *text);    // decoded characters
void web_set_load(float fraction);             // analysis task busy share (0..1)
void web_set_boot_info(const char *reason, bool unexpected, unsigned resets);
