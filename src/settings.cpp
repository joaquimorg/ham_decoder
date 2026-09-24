#include "settings.h"

#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "config.h"

#if __has_include("wifi_secrets.h")
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
    nvs_set_str(h, "ssid", g_settings.wifi_ssid);
    nvs_set_str(h, "pass", g_settings.wifi_pass);
    nvs_commit(h);
    nvs_close(h);
}
