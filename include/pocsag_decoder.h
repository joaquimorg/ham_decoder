#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

// POCSAG pager messages at 512, 1200 and 2400 baud, from the audio of an FM
// receiver. After the FM discriminator POCSAG is plain NRZ data (no audio
// tones); the three speeds are demodulated in parallel and the polarity is
// taken from the sync codeword. Codewords are corrected (BCH, up to 2 bits)
// and every message is given as one line:
//   RIC 1234567 F3 1200: text
// Receiving third-party paging messages may be restricted by law; see
// POCSAG_ENABLE in config.h.

#if POCSAG_ENABLE

void pocsag_init();
void pocsag_process(const float *x, int n);

// Copies the messages decoded since the last call (one per line,
// NUL-terminated) and clears them.
size_t pocsag_take_text(char *buf, size_t size);

uint32_t pocsag_messages();     // messages since boot
int pocsag_last_baud();         // speed of the last message (0 = none)
int64_t pocsag_last_us();       // esp_timer time of the last message (0 = none)

#if POCSAG_SELFTEST
// Decodes generated transmissions (three speeds, inverted polarity, bit
// errors, receiver filtering, noise) and logs the result.
void pocsag_selftest();
#endif

#endif
