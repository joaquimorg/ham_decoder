#pragma once

#include <stdint.h>
#include <stdbool.h>

// Wi-Fi + web interface: live spectrum/waterfall, classification, decoded CW
// and RTTY text, FAX/SSTV images and settings. Joins the Wi-Fi network in g_settings; without it (or if
// it cannot connect) it also opens the "RX-Analyzer" access point at
// http://192.168.4.1 so the network can be configured from the page.

// Spectrum rows sent to the page: bins 0..WEB_BINS-1 of the analyzer FFT
// (DSP_SAMPLE_RATE / FFT_SIZE Hz each), level in 0.5 dB steps from -120 dBFS.
#define WEB_BINS 300

#include "ftx_core.h"

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
    char ml_label[8];       // TinyML classifier
    float ml_prob;
};

enum WebTextChannel { WEB_TEXT_CW = 0, WEB_TEXT_RTTY = 1 };

void web_start();

// Producers (analysis task).
void web_push_spectrum(const uint8_t *row);    // WEB_BINS values, once per FFT frame
void web_push_status(const WebStatus &st);     // once per report
void web_push_text(WebTextChannel ch, const char *text);    // decoded characters
void web_set_load(float fraction);             // analysis task busy share (0..1)

// Decoded image (FAX, SSTV), sent to the page line by line (GET /api/img).
// channels: 1 = grey, 3 = RGB; aspect: displayed height of a line relative to
// the width of a pixel. A new image replaces the previous one: lines and the
// end sent with an older id (another decoder's image) are ignored.
#define WEB_IMG_MAX_LINE 1920    // bytes per line (width * channels): 640 px RGB
uint32_t web_image_begin(const char *title, int width, int channels, float aspect);
void web_image_line(uint32_t id, const uint8_t *px);
void web_image_end(uint32_t id);
// Rotates every held line of image `id` left by `px` pixels (manual FAX
// alignment) and gives it a new id, so pages fetch it again. Returns the new
// id (the old one if `id` is no longer current).
uint32_t web_image_rotate(uint32_t id, int px);
// Copies the image `id` (if still the current one and whole in the ring) to
// the gallery of the last WEB_GALLERY images (GET /api/gallery).
#define WEB_GALLERY 5
void web_image_archive(uint32_t id);

// Decoded FT8/FT4 message (GET /api/ftx), from the decoding task.
void web_push_ftx(const FtxMessage &m);
void web_set_boot_info(const char *reason, bool unexpected, unsigned resets);
