#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "ftx_core.h"

// What the decoders produce for the user interfaces (LCD and web page):
// spectrum rows, status, decoded text, the current image and FT8/FT4
// messages. Producers (analysis/decoder tasks) push; each interface pulls
// what it has not seen yet by sequence number, so none of them depends on
// the other being present.

// Spectrum rows: bins 0..UI_BINS-1 of the analyzer FFT
// (DSP_SAMPLE_RATE / FFT_SIZE Hz each), level in 0.5 dB steps from -120 dBFS.
#define UI_BINS 300

struct UiStatus {
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
    float psk_hz;           // PSK decoder centre, 0 = unlocked
    bool psk_active;        // squelch open
    char psk_mode[8];       // "PSK31", "PSK63", "PSK125"
    uint32_t aprs_frames;   // APRS frames since boot
    bool aprs_recent;       // a frame in the last APRS_RECENT_S seconds
    uint32_t pocsag_msgs;   // POCSAG messages since boot
    bool pocsag_recent;     // a message in the last POCSAG_RECENT_S seconds
    char ml_label[8];       // TinyML classifier
    float ml_prob;
};

enum UiTextChannel { UI_TEXT_CW = 0, UI_TEXT_RTTY = 1, UI_TEXT_PSK = 2, UI_TEXT_APRS = 3, UI_TEXT_POCSAG = 4, UI_TEXT_COUNT };

// Creates the shared state. Call before any producer runs.
void ui_hub_init();

// Producers (analysis task).
void ui_push_spectrum(const uint8_t *row);    // UI_BINS values, once per FFT frame
void ui_push_status(const UiStatus &st);      // once per report
void ui_push_text(UiTextChannel ch, const char *text);    // decoded characters
void ui_set_load(float fraction);             // analysis task busy share (0..1)
float ui_load();
void ui_set_boot_info(const char *reason, bool unexpected, unsigned resets);

// Decoded image (FAX, SSTV), line by line. channels: 1 = grey, 3 = RGB;
// aspect: displayed height of a line relative to the width of a pixel. A new
// image replaces the previous one: lines and the end sent with an older id
// (another decoder's image) are ignored.
#define UI_IMG_MAX_LINE 1920    // bytes per line (width * channels): 640 px RGB
uint32_t ui_image_begin(const char *title, int width, int channels, float aspect);
void ui_image_line(uint32_t id, const uint8_t *px);
void ui_image_end(uint32_t id);
// Rotates every held line of image `id` left by `px` pixels (manual FAX
// alignment) and gives it a new id, so viewers fetch it again. Returns the new
// id (the old one if `id` is no longer current).
uint32_t ui_image_rotate(uint32_t id, int px);
// Copies the image `id` (if still the current one and whole in the ring) to
// the gallery of the last UI_GALLERY images.
#define UI_GALLERY 5
void ui_image_archive(uint32_t id);

// Decoded FT8/FT4 message, from the decoding task.
void ui_push_ftx(const FtxMessage &m);

// ---------------------------------------------------------------------------
// Consumers.

// Copies up to `max` rows from sequence `from` on (clamped: a consumer that
// fell behind, or a new one, gets the most recent `max`). Returns the count;
// *next is the sequence after the last row.
int ui_get_rows(uint32_t from, uint8_t (*out)[UI_BINS], int max, uint32_t *next);

UiStatus ui_get_status();

// Characters of channel `ch` from sequence `from` on (a new or late reader
// gets the last 512), NUL-terminated in out[UI_TEXT_RING + 1]. Returns the count.
#define UI_TEXT_RING 2048
int ui_get_text(UiTextChannel ch, uint32_t from, char *out, uint32_t *next);

// FT8/FT4 messages from sequence `from` on (at most UI_FTX_RING).
#define UI_FTX_RING 64
int ui_get_ftx(uint32_t from, FtxMessage *out, uint32_t *next);

struct UiImageInfo {
    uint32_t id;        // 0 = no image yet
    uint32_t lines;     // lines received
    uint32_t first;     // oldest line still held
    uint16_t width;
    uint8_t channels;
    bool active;        // still receiving
    float aspect;
    char title[32];
};
UiImageInfo ui_get_image_info();
// Copies lines [first, first + n) of image `id`; false if the image changed
// or the lines are no longer held.
bool ui_get_image_lines(uint32_t id, uint32_t first, uint32_t n, uint8_t *out);

// Gallery (web page).
struct UiGalleryEntry {
    uint32_t n;         // sequence number, 0 = empty slot
    uint16_t w, h;
    int64_t utc;        // reception time (0 = clock not set)
    char title[32];
};
uint32_t ui_gallery_seq();
int ui_gallery_list(UiGalleryEntry *out);    // up to UI_GALLERY entries
// Copies bytes [off, off + n) of the RGB pixels of gallery image `n`.
// Returns the bytes copied, -1 if that image is gone.
int ui_gallery_read(uint32_t n, size_t off, uint8_t *out, size_t max);

// Boot information for the status displays.
const char *ui_boot_reason();
bool ui_boot_unexpected();
unsigned ui_boot_resets();
