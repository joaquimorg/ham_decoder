#include "settings.h"

#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "config.h"
#include "audio_monitor.h"
#include "cw_decoder.h"
#include "fax_decoder.h"
#include "sstv_decoder.h"
#include "skimmer.h"
#include "qrss.h"
#include "mfsk_decoder.h"
#include "ftx_core.h"

// Network built into the firmware for boards without a display. With the LCD
// the network is chosen on the board itself, so this file is ignored.
#if !LCD_UI && __has_include("wifi_secrets.h")
#include "wifi_secrets.h"    // optional, git-ignored: WIFI_SSID / WIFI_PASS
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

static const char *TAG = "SETTINGS";
static const char *NVS_NS = "rxcfg";

Settings g_settings;

static void set_defaults()
{
    g_settings.cw_auto_tone = true;
    g_settings.cw_tone_hz = 700.0f;
    g_settings.cw_min_contrast = CW_MIN_CONTRAST;
    g_settings.rtty_baud = RTTY_DEFAULT_BAUD;
    g_settings.rtty_polarity = 0;
    g_settings.fax_lpm = FAX_DEFAULT_LPM;
    g_settings.fax_ioc = FAX_DEFAULT_IOC;
    g_settings.fax_auto = true;
    g_settings.ftx_mode = 0;
    g_settings.sstv_adjust = false;
    g_settings.skim_enabled = true;
    g_settings.qrss_hz = QRSS_DEFAULT_HZ;
    g_settings.mfsk_mode = MFSK_OFF;
    g_settings.mfsk_tones = 32;
    g_settings.mfsk_bw = 1000;
    g_settings.mfsk_hz = MFSK_DEFAULT_HZ;
    g_settings.decoder = DEC_AUTO;
    g_settings.dec_hz = 1500;
    g_settings.web_enabled = true;
    g_settings.lcd_brightness = 80;
    g_settings.monitor_volume = MONITOR_DEFAULT_VOL;
    g_settings.language = LANG_PT;
    strlcpy(g_settings.wifi_ssid, WIFI_SSID, sizeof(g_settings.wifi_ssid));
    strlcpy(g_settings.wifi_pass, WIFI_PASS, sizeof(g_settings.wifi_pass));
}

void settings_init()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    set_defaults();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return;    // nothing saved yet

    uint8_t u8;
    uint32_t u32;
    size_t len;
    if (nvs_get_u8(h, "cw_auto", &u8) == ESP_OK)
        g_settings.cw_auto_tone = u8 != 0;
    if (nvs_get_u32(h, "cw_tone", &u32) == ESP_OK)
        g_settings.cw_tone_hz = u32 / 10.0f;
    if (nvs_get_u32(h, "cw_contr", &u32) == ESP_OK)
        g_settings.cw_min_contrast = u32 / 100.0f;
    if (nvs_get_u32(h, "rt_baud", &u32) == ESP_OK)
        g_settings.rtty_baud = u32 / 100.0f;
    if (nvs_get_u8(h, "rt_pol", &u8) == ESP_OK && u8 <= 2)
        g_settings.rtty_polarity = u8;
    if (nvs_get_u32(h, "fax_lpm", &u32) == ESP_OK && (u32 == 60 || u32 == 90 || u32 == 120 || u32 == 240))
        g_settings.fax_lpm = u32;
    if (nvs_get_u32(h, "fax_ioc", &u32) == ESP_OK && (u32 == 288 || u32 == 576))
        g_settings.fax_ioc = u32;
    if (nvs_get_u8(h, "fax_auto", &u8) == ESP_OK)
        g_settings.fax_auto = u8 != 0;
    if (nvs_get_u8(h, "ftx_mode", &u8) == ESP_OK && u8 < FTX_PROTOCOLS)
        g_settings.ftx_mode = u8;
    if (nvs_get_u8(h, "sstv_adj", &u8) == ESP_OK)
        g_settings.sstv_adjust = u8 != 0;
    if (nvs_get_u8(h, "skim_on", &u8) == ESP_OK)
        g_settings.skim_enabled = u8 != 0;
    if (nvs_get_u32(h, "qrss_hz", &u32) == ESP_OK && u32 >= 300 && u32 <= 3000)
        g_settings.qrss_hz = u32;
    if (nvs_get_u8(h, "mfsk_mode", &u8) == ESP_OK && u8 <= MFSK_CONTESTIA)
        g_settings.mfsk_mode = u8;
    if (nvs_get_u32(h, "mfsk_fmt", &u32) == ESP_OK && mfsk_valid(u32 >> 16, u32 & 0xFFFF)) {
        g_settings.mfsk_tones = u32 >> 16;
        g_settings.mfsk_bw = u32 & 0xFFFF;
    }
    if (nvs_get_u32(h, "mfsk_hz", &u32) == ESP_OK && u32 >= 300 && u32 <= 3000)
        g_settings.mfsk_hz = u32;
    if (nvs_get_u8(h, "dec_sel", &u8) == ESP_OK && u8 < DEC_COUNT)
        g_settings.decoder = u8;
    if (nvs_get_u32(h, "dec_hz", &u32) == ESP_OK && u32 >= 300 && u32 <= 3000)
        g_settings.dec_hz = u32;
    if (nvs_get_u8(h, "web_on", &u8) == ESP_OK)
        g_settings.web_enabled = u8 != 0;
    if (nvs_get_u8(h, "lcd_bl", &u8) == ESP_OK && u8 >= 5 && u8 <= 100)
        g_settings.lcd_brightness = u8;
    if (nvs_get_u8(h, "mon_vol", &u8) == ESP_OK && u8 <= 100)
        g_settings.monitor_volume = u8;
    if (nvs_get_u8(h, "lang", &u8) == ESP_OK && u8 <= LANG_EN)
        g_settings.language = u8;
    len = sizeof(g_settings.wifi_ssid);
    nvs_get_str(h, "ssid", g_settings.wifi_ssid, &len);
    len = sizeof(g_settings.wifi_pass);
    nvs_get_str(h, "pass", g_settings.wifi_pass, &len);
    nvs_close(h);

    ESP_LOGI(TAG, "CW %s %.0f Hz, contraste %.1f, RTTY %.2f bd pol %d, Wi-Fi \"%s\"",
             g_settings.cw_auto_tone ? "auto" : "manual", g_settings.cw_tone_hz,
             g_settings.cw_min_contrast, g_settings.rtty_baud, g_settings.rtty_polarity,
             g_settings.wifi_ssid);
}

unsigned settings_count_reset(bool unexpected)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return 0;
    uint32_t n = 0;
    nvs_get_u32(h, "resets", &n);
    if (unexpected) {
        n++;
        nvs_set_u32(h, "resets", n);
        nvs_commit(h);
    }
    nvs_close(h);
    return n;
}

void settings_save()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nao foi possivel abrir a NVS");
        return;
    }
    nvs_set_u8(h, "cw_auto", g_settings.cw_auto_tone ? 1 : 0);
    nvs_set_u32(h, "cw_tone", (uint32_t)(g_settings.cw_tone_hz * 10.0f + 0.5f));
    nvs_set_u32(h, "cw_contr", (uint32_t)(g_settings.cw_min_contrast * 100.0f + 0.5f));
    nvs_set_u32(h, "rt_baud", (uint32_t)(g_settings.rtty_baud * 100.0f + 0.5f));
    nvs_set_u8(h, "rt_pol", (uint8_t)g_settings.rtty_polarity);
    nvs_set_u32(h, "fax_lpm", (uint32_t)g_settings.fax_lpm);
    nvs_set_u32(h, "fax_ioc", (uint32_t)g_settings.fax_ioc);
    nvs_set_u8(h, "fax_auto", g_settings.fax_auto ? 1 : 0);
    nvs_set_u8(h, "ftx_mode", (uint8_t)g_settings.ftx_mode);
    nvs_set_u8(h, "sstv_adj", g_settings.sstv_adjust ? 1 : 0);
    nvs_set_u8(h, "skim_on", g_settings.skim_enabled ? 1 : 0);
    nvs_set_u32(h, "qrss_hz", (uint32_t)g_settings.qrss_hz);
    nvs_set_u8(h, "mfsk_mode", (uint8_t)g_settings.mfsk_mode);
    nvs_set_u32(h, "mfsk_fmt", ((uint32_t)g_settings.mfsk_tones << 16) | (uint32_t)g_settings.mfsk_bw);
    nvs_set_u32(h, "mfsk_hz", (uint32_t)g_settings.mfsk_hz);
    nvs_set_u8(h, "dec_sel", (uint8_t)g_settings.decoder);
    nvs_set_u32(h, "dec_hz", (uint32_t)g_settings.dec_hz);
    nvs_set_u8(h, "web_on", g_settings.web_enabled ? 1 : 0);
    nvs_set_u8(h, "lcd_bl", (uint8_t)g_settings.lcd_brightness);
    nvs_set_u8(h, "mon_vol", (uint8_t)g_settings.monitor_volume);
    nvs_set_u8(h, "lang", (uint8_t)g_settings.language);
    nvs_set_str(h, "ssid", g_settings.wifi_ssid);
    nvs_set_str(h, "pass", g_settings.wifi_pass);
    nvs_commit(h);
    nvs_close(h);
}

void settings_apply()
{
    cw_set_min_contrast(g_settings.cw_min_contrast);
    fax_set_lpm(g_settings.fax_lpm);
    fax_set_ioc(g_settings.fax_ioc);
    fax_set_auto(g_settings.fax_auto);
    sstv_set_auto_adjust(g_settings.sstv_adjust);
    qrss_set_center(g_settings.qrss_hz);
    if (decoder_manual()) {
        // Only the chosen decoder: Olivia/Contestia and FT8/FT4/JS8 take their
        // mode from the choice (the format and the centre of Olivia stay as set).
        const int d = g_settings.decoder;
        skimmer_set_enabled(false);
        mfsk_configure(d == DEC_OLIVIA ? MFSK_OLIVIA : d == DEC_CONTESTIA ? MFSK_CONTESTIA : MFSK_OFF,
                       g_settings.mfsk_tones, g_settings.mfsk_bw, g_settings.dec_hz);
        ftx_core_set_protocol(d >= DEC_FT8 && d <= DEC_JS8_SLOW ? (FtxProtocol)(d - DEC_FT8 + 1) : FTX_OFF);
    } else {
        skimmer_set_enabled(g_settings.skim_enabled);
        mfsk_configure((MfskMode)g_settings.mfsk_mode, g_settings.mfsk_tones, g_settings.mfsk_bw, g_settings.mfsk_hz);
        ftx_core_set_protocol((FtxProtocol)g_settings.ftx_mode);
    }
    audio_monitor_set_volume(g_settings.monitor_volume);
}

const char *decoder_name(DecoderSel d)
{
    static const char *const NAMES[DEC_COUNT] = {
        "AUTO", "CW", "RTTY", "PSK", "OLIVIA", "CONTESTIA", "FT8", "FT4",
        "JS8", "JS8 FAST", "JS8 TURBO", "JS8 SLOW", "FAX", "SSTV", "APRS", "POCSAG"
    };
    return d >= 0 && d < DEC_COUNT ? NAMES[d] : "?";
}
