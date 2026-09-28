#pragma once

#include <stdbool.h>

// Wi-Fi + web interface (optional: WEB_UI in config.h, and switched on/off in
// the settings): live spectrum/waterfall, classification, decoded CW and RTTY
// text, FAX/SSTV images and settings, all read from ui_hub. Joins the Wi-Fi
// network in g_settings; without it (or if it cannot connect) it also opens
// the "RX-Analyzer" access point at http://192.168.4.1 so the network can be
// configured from the page.

// Starts Wi-Fi and the web server in the background (returns at once).
void web_start();

// Wi-Fi state for the LCD status bar.
struct WebNetInfo {
    bool started;
    bool connected;         // joined the configured network
    bool ap;                // fallback access point open
    int rssi;               // dBm, 0 when not connected
    char ip[16];
};
WebNetInfo web_net_info();
