#pragma once

#include <stdbool.h>

// Runtime settings, persisted in NVS and editable from the web page.
struct Settings {
    bool cw_auto_tone;          // follow the analyzer's narrow tone
    float cw_tone_hz;           // manual CW tone when cw_auto_tone is false
    float cw_min_contrast;      // mark/space ratio needed to decode
    float rtty_baud;
    int rtty_polarity;          // RttyPolarity: 0 auto, 1 normal, 2 reverse
    char wifi_ssid[33];
    char wifi_pass[65];
};

extern Settings g_settings;

// Initialises NVS and loads the settings (defaults from config.h and, if
// present, include/wifi_secrets.h). Call first in app_main.
void settings_init();

void settings_save();

// Adds one to the persistent count of unexpected resets (crash, watchdog,
// brownout) when `unexpected`, and returns the count.
unsigned settings_count_reset(bool unexpected);
