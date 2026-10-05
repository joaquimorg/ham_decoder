#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ft8/decode.h>

// JS8 (JS8Call): FT8's 8-FSK frame (79 symbols, three 7x7 Costas arrays,
// 58 data symbols) with its own Costas arrays, no Gray code, a (174,87) LDPC
// code and a 12-bit CRC. A frame carries 72 bits (12 characters of a 64-sign
// alphabet) plus 3 transmission-type bits; JS8Call packs heartbeats, CQs,
// directed commands and free text into them (text compressed with a word
// dictionary, JSC). The waterfall and the candidate search reuse ft8_lib.

enum Js8Submode { JS8_NORMAL = 0, JS8_FAST = 1, JS8_TURBO = 2, JS8_SLOW = 3 };

struct Js8SubmodeInfo {
    const char *name;
    float symbol_period;    // s
    float slot;             // s
    float start_delay;      // s into the slot
    bool original_costas;   // Normal keeps the original arrays
};

const Js8SubmodeInfo &js8_submode(Js8Submode m);

// Candidates in a waterfall (best first), as ftx_find_candidates().
int js8_find_candidates(const ftx_waterfall_t *wf, bool original_costas, int max, ftx_candidate_t *out,
                        int min_score);

// Decodes a candidate: the 72 frame bits (9 bytes, MSB first) and the
// 3 transmission-type bits. False if the LDPC or the CRC fails.
bool js8_decode_candidate(const ftx_waterfall_t *wf, const ftx_candidate_t *cand, int max_iterations,
                          uint8_t payload[9], int *i3);

// Text of a frame as JS8Call shows it ("CT1ABC: @HB HEARTBEAT IM58",
// "CT1ABC: CT2XYZ SNR -07", free text...).
void js8_unpack(const uint8_t payload[9], int i3, char *out, size_t size);

// Tones of a frame (79 symbols), for tests and the SNR estimate.
void js8_encode(const uint8_t payload[9], int i3, bool original_costas, uint8_t tones[79]);

// The JSC dictionary (data/jsc_dict.bin): on the board it is embedded in the
// firmware and set at start; without it compressed text shows as "<JSC>".
void js8_set_dictionary(const uint8_t *blob, size_t size);
