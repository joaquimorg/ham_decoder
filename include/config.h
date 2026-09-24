#pragma once

#include "sdkconfig.h"

// RX Analyzer - hardware configuration
// Target: ESP32-S3 DevKitC-1 N16R8 (PSRAM needed for FAX/SSTV/FT8).

#if CONFIG_IDF_TARGET_ESP32S3
#define BOARD_NAME      "ESP32-S3"
#else
#error "Unsupported target: add it in config.h"
#endif

// Audio source. The internal ADC decodes 30 WPM CW without errors; both
// PCM1808 modules tested corrupt the top bits of the samples (see docs/HARDWARE.md).
#define AUDIO_SRC_PCM1808   0
#define AUDIO_SRC_ADC       1
#define AUDIO_SOURCE        AUDIO_SRC_ADC

// PCM1808 over I2S: ESP32 master (MCLK 256 fs), PCM1808 slave
// (MD0 = MD1 = FMT = GND). 22 ohm series resistors on all four lines at the
// ESP32 side, and MCLK kept away from the others - without them the clock
// edges ring and the PCM1808 loses sync (output stuck at 0).
#define PCM_MCLK_GPIO       8
#define PCM_BCK_GPIO        4
#define PCM_WS_GPIO         6
#define PCM_DIN_GPIO        15
#define PCM_CHANNEL         0       // 0 = LIN, 1 = RIN
// 1 = MCLK from an integer divider (ESP32-S3 has no APLL): 160 MHz / 13 =
//     12.308 MHz, Fs = 48077 Hz. 0 = exact 12.288 MHz via a fractional
//     divider (one stretched cycle every 48), Fs = 48000 Hz.
//     Tried: no effect on the PCM1808 error rate.
#define PCM_MCLK_INTEGER_DIV 0
// 1 = invert BCK: moves the PCM1808 DOUT change half a BCK period relative to
//     the ESP32 sampling edge (tried; no effect on the error rate).
#define PCM_BCK_INVERT      0
// 1 = repair corrupted samples (top bits of the word wrong, low 16 bits
//     intact) and log counts plus raw examples every 5 s.
#define PCM_GLITCH_FIX      1

// Internal ADC1 (alternative): audio -> 1 uF -> GPIO1, biased at 3V3/2 by
// 2 x 10 k. ADC1 only (ADC2 is shared with Wi-Fi). 12 dB attenuation:
// ~0..3.1 V range, so the 1.65 V bias sits near mid-scale; max ~2.8 Vpp.
#define ADC_INPUT_GPIO      1
#define ADC_INPUT_ATTEN     ADC_ATTEN_DB_12

// Capture rate; any multiple of DSP_SAMPLE_RATE works (24 kHz was tried to
// halve BCK/MCLK and did not change the PCM1808 error rate).
#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_BLOCK_SAMPLES 256
// Both sources scale their full scale to +-AUDIO_FULL_SCALE (24-bit).
#define AUDIO_FULL_SCALE    8388608.0f

// DSP runs at 12 kHz (radio audio lives below ~3.5 kHz); the capture rate
// is decimated down to it by DECIM_FACTOR.
#define DSP_SAMPLE_RATE     12000
#define DECIM_FACTOR        (AUDIO_SAMPLE_RATE / DSP_SAMPLE_RATE)
#if AUDIO_SAMPLE_RATE % DSP_SAMPLE_RATE
#error "AUDIO_SAMPLE_RATE must be a multiple of DSP_SAMPLE_RATE"
#endif
#define FFT_SIZE            1024    // 11.7 Hz/bin, 85 ms per frame at 12 kHz
#define FFT_FRAMES_PER_REPORT 12    // ~1 s averaged per report line
#define DSP_NUM_BUFFERS     3

// Spectrum analysis window
#define SPECTRUM_MIN_HZ     100
#define SPECTRUM_MAX_HZ     3500

// Detection thresholds
#define SIGNAL_MIN_SNR_DB   10.0f   // strongest peak above median floor
#define PEAK_MIN_DB         10.0f   // candidate peak above floor
#define PEAK_MIN_SEP_HZ     40.0f
#define KEYING_MIN_DB       8.0f    // envelope p90-p10 swing for on/off keying

// CW decoder
#define CW_TICK_MS          5       // envelope step; window ~dit/3 (20 WPM: 20 ms, ~50 Hz)
#define CW_DEBOUNCE_TICKS   2       // key changes shorter than this are ignored...
#define CW_DEBOUNCE_DIT     0.4f    // ...or shorter than this fraction of a dit,
#define CW_DEBOUNCE_MAX_TICKS 3     // ...capped at 15 ms
#define CW_SHORT_SHARE      0.4f    // share of too-short elements that means the
                                    // speed estimate (not noise) is wrong
#define CW_MIN_CONTRAST     5.0f    // mark/space level ratio needed (~14 dB); lower
                                    // lets pure noise decode as random E/T/I
#define CW_RETUNE_HZ        10.0f   // retune when the tone moves more than this
#define CW_KEYED_EXTRA_SNR_DB 10.0f // keyed tone this far above SIGNAL_MIN_SNR_DB counts
                                    // as CW even when harmonics widen the spectrum
#define CW_UNLOCK_REPORTS   5       // seconds without a narrow tone before unlocking
#define CW_DEBUG            0       // 1 = print keying statistics under each line (SERIAL_REPORT 2)
#define CW_TEXT_MAX         64      // decoded characters buffered per report line

// RTTY decoder (Baudot/ITA2)
#define RTTY_DEFAULT_BAUD   45.45f
#define RTTY_RETUNE_HZ      15.0f   // retune when a tone moves more than this
#define RTTY_UNLOCK_REPORTS 5       // seconds without FSK before unlocking
#define RTTY_TEXT_MAX       64      // decoded characters buffered per report

// Image modes (FAX, SSTV): shared FM discriminator (fm_demod.cpp)
#define FM_CENTER_HZ        1700.0f // SSTV 1100..2300 Hz, FAX 1500..2300 Hz
#define FM_CUTOFF_HZ        1000.0f // low-pass after mixing: 700..2700 Hz
#define FM_TAPS             63

// HF FAX (WEFAX): 1500 Hz black, 2300 Hz white
#define FAX_WIDTH           904     // pixels per line (IOC 576 has pi*576 = 1810)
#define FAX_DEFAULT_LPM     120
#define FAX_DEFAULT_IOC     576
#define FAX_MAX_LINES       3000    // an image ends here if no stop tone comes
#define FAX_CLOCK_PPM       0.0f    // sample clock correction (slant)

// FT8 / FT4 (ft8_lib): UTC time from NTP (pool.ntp.org) or from the web page
#define FTX_NTP_SERVER      "pool.ntp.org"
#define FTX_TASK_STACK      12288   // decoding task (core 0)

// Wi-Fi radio. Power save (modem sleep) switches the radio on and off in
// bursts that show up as noise and spurs on the internal ADC and delay
// packets, so it is disabled. TX power in 0.25 dBm units (8..84): 78 = 19.5 dBm.
// Lower values reduce interference on the audio input but, with a weak link,
// made the web page slow and unstable (11 and 15 dBm were tried).
#define WIFI_TX_POWER_QDBM  78

// Capture: > 0 records this many seconds of the DSP stream (what the decoders
// see) once the CW decoder locks, then dumps it over the console for
// tools/capture_to_wav.py. 0 = off.
#define CAPTURE_SECONDS     0

// Serial console output (the web page shows everything):
//   0 = only warnings/errors and the web address
//   1 = decoded CW text as it arrives + a line when the kind of signal changes
//   2 = full text waterfall line every second (plus CW_DEBUG / DIAG_VERBOSE)
#define SERIAL_REPORT       1

// Text waterfall on the console (SERIAL_REPORT 2)
#define WATERFALL_COLS          70
#define WATERFALL_HZ_PER_COL    50
#define WATERFALL_RULER_EVERY   20

// TinyML classifier: 1 = print the feature vector of every report as
// "ML_F,<label>,<values>" to record real examples for tools/ml/log_to_npz.py.
#define ML_LOG_FEATURES     0
#define ML_LOG_LABEL        "?"

// 1 = print floor/peaks/envelope details under each waterfall line
#define DIAG_VERBOSE        0
