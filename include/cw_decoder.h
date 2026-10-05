#pragma once

#include <stddef.h>

#include "config.h"

// Morse (CW) decoder. Runs continuously on the DSP-rate stream (DSP_SAMPLE_RATE,
// float, full scale = 1.0) and decodes the tone it is told to follow.

class CwReceiver {
public:
    CwReceiver();

    // Tone to follow; 0 unlocks and resets the decoder.
    void set_tone(float hz);
    // Mark/space level ratio needed before decoding (default CW_MIN_CONTRAST).
    void set_min_contrast(float ratio);

    float tone_hz() const;    // 0 = unlocked
    float wpm() const;        // speed estimate
    bool usable() const;      // mark/space contrast enough to decode
    float min_contrast_value() const { return min_contrast; }

    void process(const float *x, int n);

    // Copies the text decoded since the last call (NUL-terminated) and clears it.
    size_t take_text(char *buf, size_t size);

    // One line of keying statistics since the last call (for CW_DEBUG).
    size_t debug_line(char *buf, size_t size);

private:
    static constexpr int WIN_MAX = 4;
    static constexpr int HIST = (int)(1.5f * 1000 / CW_TICK_MS);

    void emit(char c);
    void flush_symbol();
    float learn(float est, float len);
    void keep_ratio();
    void end_mark(int len);
    void end_space(int len);
    void space_tick(int len);
    float percentile(int n, float p);
    void update_levels();
    void envelope_tick(float mag);
    void reset_state();

    // Tuning (oscillator) and per-tick quadrature sums.
    float tone = 0.0f;
    float osc_r = 1.0f, osc_i = 0.0f, rot_r = 1.0f, rot_i = 0.0f;
    float acc_i = 0.0f, acc_q = 0.0f;
    int acc_n = 0;
    float ring_i[WIN_MAX], ring_q[WIN_MAX];
    int ring_pos = 0;

    // Envelope history and levels.
    float hist[HIST];
    float sorted_hist[HIST];
    int hist_len = 0, hist_pos = 0;
    float floor_level = 0.0f, top_level = 0.0f;
    int ticks = 0;

    // Keying state (in ticks).
    bool key_down = false;
    int pending = 0;          // ticks the raw decision has disagreed with key_down
    int run_ticks = 0;        // length of the current mark or space
    bool carrier = false;     // current mark is too long to be Morse

    float dit_ticks, dah_ticks;
    float min_contrast = CW_MIN_CONTRAST;
    float short_share = 0.0f;

    char symbol[8];
    int symbol_len = 0;
    bool symbol_overflow = false;     // more elements than any Morse character
    bool word_gap_sent = true;

    // Per-report statistics for CW_DEBUG.
    struct {
        int marks, dahs, carriers, chars, usable_ticks, ticks;
        int mark_min, mark_max, space_min;
    } stats = {};

    char text[CW_TEXT_MAX + 1];
    size_t text_len = 0;
};

// The main decoder (the analyzer tunes it).
void cw_init();
void cw_set_tone(float hz);
void cw_set_min_contrast(float ratio);
float cw_tone_hz();
float cw_wpm();
void cw_process(const float *x, int n);
size_t cw_take_text(char *buf, size_t size);
size_t cw_debug_line(char *buf, size_t size);
