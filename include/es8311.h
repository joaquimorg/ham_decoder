#pragma once

#include "config.h"

#if defined(BOARD_FNK0104S) && (AUDIO_SOURCE == AUDIO_SRC_ES8311 || AUDIO_MONITOR)

#include "driver/i2s_std.h"

// Freenove FNK0104S's ES8311 codec: I2S master (MCLK 256 fs, 48 kHz, 32-bit
// slots) plus the codec registers over the shared I2C bus. One I2S port serves
// both directions: the receive channel only exists with AUDIO_SRC_ES8311, the
// transmit channel with AUDIO_MONITOR.

// Starts the I2S clocks and configures the codec. Safe to call more than once
// (the audio source and the monitor each call it). Returns false if the codec
// does not answer on I2C.
bool es8311_start();

i2s_chan_handle_t es8311_rx();    // nullptr unless the codec is the audio source
i2s_chan_handle_t es8311_tx();    // nullptr unless AUDIO_MONITOR

// Monitor volume, 0..100 %: 0 mutes the DAC and switches the speaker amplifier
// off. May be called before es8311_start(); it is applied once the codec is up.
void es8311_set_volume(int percent);

#endif
