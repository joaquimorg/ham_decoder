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
    int fail_reason;        // last STA disconnect reason (wifi_err_reason_t), 0 when connected
};
WebNetInfo web_net_info();

// Network setup from the LCD (WEB_UI builds, once the Wi-Fi is started).
struct WebScanEntry {
    char ssid[33];
    int rssi;               // dBm
    bool secure;            // needs a password
};
#define WEB_SCAN_MAX 16

// Starts a scan in the background; false if Wi-Fi is not ready.
bool web_scan_start();
// 1 while scanning, 2 when finished (entries valid, strongest first, one per
// SSID), 0 when none was started. `*n` receives the number of entries.
int web_scan_result(WebScanEntry *out, int max, int *n);
// Saves the network in the settings and joins it now (no restart).
bool web_connect(const char *ssid, const char *pass);
