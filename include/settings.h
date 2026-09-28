#pragma once

#include <stdbool.h>

// Runtime settings, persisted in NVS and editable from the web page.
enum Language { LANG_PT = 0, LANG_EN = 1 };    // LCD interface

struct Settings {
    bool cw_auto_tone;          // follow the analyzer's narrow tone
    float cw_tone_hz;           // manual CW tone when cw_auto_tone is false
    float cw_min_contrast;      // mark/space ratio needed to decode
    float rtty_baud;
    int rtty_polarity;          // RttyPolarity: 0 auto, 1 normal, 2 reverse
    int fax_lpm;                // 60, 90, 120, 240
    int fax_ioc;                // 576, 288
    bool fax_auto;              // start on the APT start tone
    int ftx_mode;               // FtxProtocol: 0 off, 1 FT8, 2 FT4
    bool sstv_adjust;           // redraw SSTV images with the auto adjust
    bool web_enabled;           // Wi-Fi + web page (WEB_UI builds only)
    int lcd_brightness;         // backlight, 5..100 %
    int language;               // Language
    char wifi_ssid[33];
    char wifi_pass[65];
};

extern Settings g_settings;

// Initialises NVS and loads the settings (defaults from config.h and, if
// present, include/wifi_secrets.h). Call first in app_main.
void settings_init();

void settings_save();

// Passes the decoder settings (CW, FAX, SSTV, FT8/FT4) on to the decoders.
// The analyzer reads the rest of g_settings directly.
void settings_apply();

// Adds one to the persistent count of unexpected resets (crash, watchdog,
// brownout) when `unexpected`, and returns the count.
unsigned settings_count_reset(bool unexpected);
