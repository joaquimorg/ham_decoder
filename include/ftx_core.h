#pragma once

#include <stdint.h>

// FT8/FT4 on top of ft8_lib: cuts the audio into UTC time slots, builds each
// slot's waterfall (double buffered: one slot fills while the previous one is
// decoded) and decodes it. No ESP-IDF calls, so it also builds on a PC for
// tools/ftx_test (see ftx_decoder.cpp for the firmware side).

enum FtxProtocol { FTX_OFF = 0, FTX_FT8 = 1, FTX_FT4 = 2 };

struct FtxMessage {
    double slot_start;    // UTC seconds at the start of the slot
    float snr_db;         // in 2500 Hz, like WSJT-X
    float dt;             // seconds from the nominal start (0.5 s into the slot)
    float freq_hz;        // audio frequency of the lowest tone
    char text[36];
};

typedef void (*FtxMessageCb)(const FtxMessage &msg, void *ctx);

// Allocates the FT8 and FT4 waterfalls (~0.5 MB, PSRAM on the board).
bool ftx_core_init(int sample_rate);

// Any task; applied by the next ftx_core_feed().
void ftx_core_set_protocol(FtxProtocol p);
FtxProtocol ftx_core_protocol();

// Feeds n samples (n well under a slot), x[0] taken at UTC time t0 seconds.
// Returns a handle when a slot's waterfall has just been completed (its start
// in *slot_start), -1 otherwise. The handle goes to ftx_core_decode() (from
// any task) and then to ftx_core_release().
int ftx_core_feed(const float *x, int n, double t0, double *slot_start);

// Decodes a completed slot; calls cb once per distinct message. Returns the
// number of messages.
int ftx_core_decode(int handle, double slot_start, FtxMessageCb cb, void *ctx);
void ftx_core_release(int handle);

int ftx_core_skipped();    // slots dropped because both buffers were busy
