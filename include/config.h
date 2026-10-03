#pragma once

#include "sdkconfig.h"

// RX Analyzer - hardware configuration
// Boards (PlatformIO env sets BOARD_FNK0104S; otherwise the DevKitC):
//   Freenove FNK0104S: ESP32-S3R8 + 16 MB flash, 4" ST7796 480x320 LCD,
//                      FT6336U touch, ES8311 codec (see docs/HARDWARE.md)
//   ESP32-S3 DevKitC-1 N16R8
// Both have 8 MB PSRAM, needed for FAX/SSTV/FT8.

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Unsupported target: add it in config.h"
#elif defined(BOARD_FNK0104S)
#define BOARD_NAME      "Freenove FNK0104S"
#else
#define BOARD_NAME      "ESP32-S3 DevKitC"
#endif

// Audio source. The internal ADC decodes 30 WPM CW without errors; both
// PCM1808 modules tested corrupt the top bits of the samples (see docs/HARDWARE.md).
// Chosen at build time: the PlatformIO env passes -DRX_AUDIO=ADC|PCM1808|ES8311
// to CMake (src/CMakeLists.txt turns it into RX_AUDIO_*); none = the line below.
#define AUDIO_SRC_PCM1808   0
#define AUDIO_SRC_ADC       1
#define AUDIO_SRC_ES8311    2       // FNK0104S: line input on the codec's MIC1P (L3 removed)
#if defined(RX_AUDIO_ES8311)
#define AUDIO_SOURCE        AUDIO_SRC_ES8311
#elif defined(RX_AUDIO_PCM1808)
#define AUDIO_SOURCE        AUDIO_SRC_PCM1808
#elif defined(RX_AUDIO_ADC)
#define AUDIO_SOURCE        AUDIO_SRC_ADC
#else
#define AUDIO_SOURCE        AUDIO_SRC_ADC
#endif
#if defined(BOARD_FNK0104S) && AUDIO_SOURCE == AUDIO_SRC_PCM1808
#error "FNK0104S: the I2S pins go to the on-board ES8311 codec"
#endif
#if !defined(BOARD_FNK0104S) && AUDIO_SOURCE == AUDIO_SRC_ES8311
#error "ES8311 audio source: FNK0104S only"
#endif

// ES8311 codec (FNK0104S). The ESP32 is the I2S master (MCLK 256 fs, 32-bit
// slots), the codec a slave. Same I2S for the input (ADC -> ESP) and the
// monitor output (ESP -> DAC -> SC8002B -> speaker).
#define ES8311_ADDR         0x18
#define ES8311_MCLK_GPIO    4
#define ES8311_BCK_GPIO     5
#define ES8311_WS_GPIO      7
#define ES8311_DIN_GPIO     6       // codec ADC -> ESP
#define ES8311_DOUT_GPIO    8       // ESP -> codec DAC
#define ES8311_PA_GPIO      1       // SC8002B enable, active low (pull-up = off)
#define ES8311_CHANNEL      0       // slot read from the codec: 0 = left, 1 = right
// Input gain: analog PGA 0..10 (3 dB steps) plus ADC scale 0..7 (6 dB steps).
// Start low and raise it until the loudest signal stays below clipping.
#define ES8311_MIC_PGA      0
#define ES8311_ADC_SCALE    4

// Monitor: the received audio (as the analyzer hears it) on the board's speaker,
// with a volume setting (LCD and web page). Works with any audio source.
#if defined(BOARD_FNK0104S) && !defined(RX_NO_MONITOR)
#define AUDIO_MONITOR       1
#else
#define AUDIO_MONITOR       0
#endif
// Speaker band (Hz): the monitor plays a band-pass of the received audio. The
// speaker leaks back into the codec input; above the voice band that loop adds
// noise and a fast tremolo. 0 disables a corner.
#define MONITOR_LOW_HZ      200
#define MONITOR_HIGH_HZ     3500
// Speaker noise reduction (spectral, see noise_reduce.h): attenuates the
// background noise between and under the signal by up to this many dB.
#define MONITOR_NR          1
#define MONITOR_NR_DEPTH_DB 15.0f
// Speaker noise gate: mutes the monitor while there is no input, so the codec's
// noise floor is not amplified. Levels are the band-passed mean |x| in dBFS:
// opens above GATE_OPEN, closes below GATE_CLOSE (after 150 ms). 0 = no gate.
#define MONITOR_GATE        1
#define MONITOR_GATE_OPEN   -80.0f
#define MONITOR_GATE_CLOSE  -84.0f
#define MONITOR_DEFAULT_VOL 40      // %, 0 = off (amplifier disabled)

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
// FNK0104S: GPIO2 (ADC1_CH1, header P3 pin 1); its GPIO1 is the speaker
// amplifier enable (active low, pulled up = off).
#if defined(BOARD_FNK0104S)
#define ADC_INPUT_GPIO      2
#else
#define ADC_INPUT_GPIO      1
#endif
#define ADC_INPUT_ATTEN     ADC_ATTEN_DB_12

// Capture rate; any multiple of DSP_SAMPLE_RATE works (24 kHz was tried to
// halve BCK/MCLK and did not change the PCM1808 error rate).
// The ES8311 samples straight at the DSP rate: its own decimation filter is the
// anti-alias filter (it also rejects the backlight PWM at 24 kHz, which at
// 48 kHz landed on fs/2), and capture plus monitor do a quarter of the work.
// Blocks of ~5.3 ms either way.
#if AUDIO_SOURCE == AUDIO_SRC_ES8311
#define AUDIO_SAMPLE_RATE   12000
#define AUDIO_BLOCK_SAMPLES 64
#else
#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_BLOCK_SAMPLES 256
#endif
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

// PSK31 / PSK63 / PSK125 (psk_decoder.h).
#define PSK_TEXT_MAX        128     // decoded characters buffered per report (PSK125 is fast)
#define PSK_UNLOCK_REPORTS  5       // seconds without PSK before unlocking
#define PSK_SELFTEST        0       // 1 = decode generated signals at boot and log the result

// APRS / AX.25 1200 baud (aprs_decoder.h).
#define APRS_TEXT_MAX       1024    // decoded frames buffered per report
#define APRS_RECENT_S       10      // "APRS" shown as live this long after a frame
#define APRS_SELFTEST       0       // 1 = decode generated AFSK frames at boot and log the result

// POCSAG pagers 512/1200/2400 baud (pocsag_decoder.h). Receiving and showing
// third-party paging messages may be restricted by law where you are (in
// Portugal, interception of communications not meant for the public is): 0
// leaves the decoder out of the firmware.
#define POCSAG_ENABLE       1
#define POCSAG_TEXT_MAX     1024    // decoded messages buffered per report
#define POCSAG_RECENT_S     10      // "POCSAG" shown as live this long after a message
#define POCSAG_SELFTEST     0       // 1 = decode generated transmissions at boot and log the result

// NAVTEX / SITOR-B (navtex_decoder.h): 100 baud, 170 Hz shift.
#define NAVTEX_TEXT_MAX     256     // decoded characters buffered per report
#define NAVTEX_SELFTEST     0       // 1 = decode a generated transmission at boot and log the result

// Feld-Hell (hell_decoder.h): columns for the interfaces, on the manual CW
// tone or the strongest peak.
#define HELL_SELFTEST       0       // 1 = check a generated pixel pattern at boot and log the result

// DTMF and CTCSS (tone_decoder.h).
#define TONES_TEXT_MAX      512     // text buffered per report
#define TONES_SELFTEST      0       // 1 = decode generated tones at boot and log the result

// Image modes (FAX, SSTV): shared FM discriminator (fm_demod.cpp)
#define FM_CENTER_HZ        1700.0f // SSTV 1100..2300 Hz, FAX 1500..2300 Hz
#define FM_CUTOFF_HZ        1000.0f // low-pass after mixing: 700..2700 Hz
#define FM_TAPS             63

// HF FAX (WEFAX): 1500 Hz black, 2300 Hz white
#define FAX_WIDTH           904     // pixels per line (IOC 576 has pi*576 = 1810)
#define FAX_DEFAULT_LPM     120
#define FAX_DEFAULT_IOC     576
#define FAX_MAX_LINES       3000    // an image ends here if no stop tone comes
#define FAX_CLOCK_PPM       0.0f    // extra slant correction on top of the measured rate
// Noise rejection on the image path (tools/fax_sim.py has the same values).
#define FAX_SQ_ENV_TC       0.5f    // s: time constant of the mean carrier amplitude
#define FAX_SQ_SMOOTH       0.002f  // s: smoothing of the amplitude compared with it
#define FAX_SQ_LEVEL        0.2f    // below this share of the mean amplitude: white
// Line length tracking and jump resync, from the content of each line against
// the last line it matched (a noisy line is not used as the reference).
#define FAX_TRK_LAGS        12      // line pairs per slope estimate
#define FAX_TRK_MAX_LAG     3       // px: drift counted for the slope
#define FAX_TRK_MIN_STD     0.08f   // lines flatter than this carry no information
#define FAX_TRK_DEADBAND    0.05f   // px per line
#define FAX_TRK_GAIN        0.7f
#define FAX_TRK_LIMIT       0.005f  // line length within 0.5% of the nominal
#define FAX_REF_CORR        0.6f    // a line this similar to the reference becomes the reference
#define FAX_REF_AGE         8       // lines
#define FAX_JUMP_MAX_LAG    12      // px searched either way
#define FAX_JUMP_MIN        2.0f    // px: a bigger shift is a jump of the signal, not a slope
#define FAX_JUMP_MIN_CORR   0.4f
#define FAX_JUMP_MARGIN     0.15f   // correlation over the zero-shift one

// FT8 / FT4 (ft8_lib): UTC time from NTP (pool.ntp.org) or from the web page
#define FTX_NTP_SERVER      "pool.ntp.org"
#define FTX_TASK_STACK      12288   // decoding task (core 0)

// User interfaces. Both read the decoders' output from ui_hub.
// WEB_UI: 1 = build the Wi-Fi + web page (then switched on/off in the
// settings, on the LCD or in NVS); 0 = leave it out (no Wi-Fi at all).
// Without Wi-Fi there is no NTP time for FT8/FT4 (the board has no RTC).
#define WEB_UI              1
// LCD + touch (Freenove FNK0104S only): ST7796 480x320 over SPI, FT6336U.
#if defined(BOARD_FNK0104S)
#define LCD_UI              1
#else
#define LCD_UI              0
#endif
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_PIN_SCK         12
#define LCD_PIN_MOSI        11
#define LCD_PIN_MISO        13
#define LCD_PIN_CS          10
#define LCD_PIN_DC          46
#define LCD_PIN_BL          45      // backlight, active high (BSS138), PWM
#define LCD_SPI_HZ          (80 * 1000 * 1000)    // as Freenove's TFT_eSPI setup (IOMUX pins)
#define LCD_H_RES           480     // landscape
#define LCD_V_RES           320
// I2C bus shared by the touch controller and the ES8311 codec.
#define BOARD_I2C_SDA       16
#define BOARD_I2C_SCL       15
#define TOUCH_PIN_INT       17
#define TOUCH_PIN_RST       18
#define LCD_TASK_CORE       0       // with Wi-Fi; audio and analysis own core 1

// Wi-Fi radio. Power save (modem sleep) switches the radio on and off in
// bursts that show up as noise and spurs on the internal ADC and delay
// packets, so it is disabled. TX power in 0.25 dBm units (8..84): 78 = 19.5 dBm.
// Lower values reduce interference on the audio input but, with a weak link,
// made the web page slow and unstable (11 and 15 dBm were tried).
#define WIFI_TX_POWER_QDBM  78

// Capture: > 0 records this many seconds of the DSP stream (what the decoders
// see) once a decoder locks (CW, RTTY, FAX, SSTV), then dumps it over the console for
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
