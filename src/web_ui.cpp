#include "web_ui.h"
#include "ui_hub.h"
#include "config.h"

#if WEB_UI

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

#include "config.h"
#include "settings.h"
#include "cw_decoder.h"
#include "fax_decoder.h"
#include "sstv_decoder.h"
#include "ftx_decoder.h"
#include "analyzer.h"
#include "qrss.h"
#include "mfsk_decoder.h"
#include "esp_netif_sntp.h"
#include <sys/time.h>
#include <time.h>

static const char *TAG = "WEB";

#include "web_page.inc"    // const char WEB_PAGE[]

#define ROWS 64                 // spectrum rows a client can get at once
#define TEXT_RING UI_TEXT_RING
#define FTX_RING UI_FTX_RING
#define WEB_QRSS_MAX 48         // QRSS columns a client can get at once (66 s)
#define IMG_TITLE 32

// ---------------------------------------------------------------------------
// Wi-Fi

#define WIFI_CONNECTED_BIT BIT0
#define STA_TIMEOUT_MS     60000    // before also opening the fallback access point
#define STA_RETRY_MS       3000
#define AP_SSID            "RX-Analyzer"
#define AP_PASS            "rxanalyzer"
#define HOSTNAME           "rx-analyzer"

static EventGroupHandle_t wifi_events;
static esp_netif_t *sta_netif = nullptr;
static bool ap_active = false;
static esp_timer_handle_t retry_timer = nullptr;
static volatile bool wifi_ready = false;        // esp_wifi_start() done
static volatile bool scanning = false;
static volatile int scan_state = 0;             // 0 none, 1 scanning, 2 done
static WebScanEntry scan_list[WEB_SCAN_MAX];
static int scan_count = 0;
static volatile int fail_reason = 0;

static void retry_connect(void *)
{
    if (scanning)    // a scan cannot start or finish while joining
        esp_timer_start_once(retry_timer, STA_RETRY_MS * 1000ULL);
    else
        esp_wifi_connect();
}
static char ip_str[16] = "";

static void collect_scan()
{
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    static wifi_ap_record_t recs[24];
    if (num > 24)
        num = 24;
    esp_wifi_scan_get_ap_records(&num, recs);    // strongest first
    scan_count = 0;
    for (int i = 0; i < num && scan_count < WEB_SCAN_MAX; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0])
            continue;    // hidden
        bool dup = false;
        for (int k = 0; k < scan_count; k++)
            dup |= strcmp(scan_list[k].ssid, ssid) == 0;
        if (dup)
            continue;
        WebScanEntry &e = scan_list[scan_count++];
        strlcpy(e.ssid, ssid, sizeof(e.ssid));
        e.rssi = recs[i].rssi;
        e.secure = recs[i].authmode != WIFI_AUTH_OPEN;
    }
}

static void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_settings.wifi_ssid[0])
            esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        collect_scan();
        scanning = false;
        scan_state = 2;
        if (g_settings.wifi_ssid[0] && !ip_str[0])
            esp_wifi_connect();    // resume the join the scan interrupted
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        fail_reason = ev->reason;
        ESP_LOGW(TAG, "Wi-Fi desligado (motivo %d, RSSI %d dBm)", ev->reason, ev->rssi);
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        ip_str[0] = 0;
        // Keep retrying in the background, without blocking the event loop.
        if (g_settings.wifi_ssid[0] && retry_timer)
            esp_timer_start_once(retry_timer, STA_RETRY_MS * 1000ULL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        fail_reason = 0;
        ESP_LOGW(TAG, "ligado a \"%s\": http://%s/", g_settings.wifi_ssid, ip_str);
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
        static bool sntp_started = false;    // UTC for FT8/FT4
        if (!sntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(FTX_NTP_SERVER);
            sntp_started = esp_netif_sntp_init(&cfg) == ESP_OK;
        }
    }
}

static void set_sta_config()
{
    wifi_config_t sta = {};
    strlcpy((char *)sta.sta.ssid, g_settings.wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, g_settings.wifi_pass, sizeof(sta.sta.password));
    // Scan every channel and join the strongest AP with this SSID (repeaters).
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
}

static void start_access_point()
{
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {};
    strlcpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 3;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ap_active = true;
    ESP_LOGW(TAG, "ponto de acesso \"%s\" (password \"%s\"): http://192.168.4.1/", AP_SSID, AP_PASS);
}

static bool wifi_start()
{
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    const esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        // Usually the internal RAM: without this check the abort rebooted
        // the device in a loop.
        ESP_LOGE(TAG, "esp_wifi_init: %s (RAM interna livre %u B); sem Wi-Fi nem pagina web", esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, nullptr));

    esp_timer_create_args_t targs = {};
    targs.callback = retry_connect;
    targs.name = "wifi_retry";
    ESP_ERROR_CHECK(esp_timer_create(&targs, &retry_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    set_sta_config();
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_ready = true;
    // Radio always on (no modem-sleep bursts on the ADC, steadier latency)
    // and at reduced power.
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);

    if (!g_settings.wifi_ssid[0]) {
        ESP_LOGW(TAG, "sem rede Wi-Fi configurada");
        start_access_point();
        return true;
    }
    ESP_LOGI(TAG, "a ligar a \"%s\"...", g_settings.wifi_ssid);
    const EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                                 pdMS_TO_TICKS(STA_TIMEOUT_MS));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "nao liga a \"%s\" (continua a tentar)", g_settings.wifi_ssid);
        start_access_point();
    }
    return true;
}

// ---------------------------------------------------------------------------
// HTTP helpers

static void base64_encode(const uint8_t *in, int len, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 |
                           (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) |
                           (i + 2 < len ? (uint32_t)in[i + 2] : 0);
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[o] = 0;
}

// Decodes application/x-www-form-urlencoded values in place.
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, nullptr, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

static bool form_value(const char *body, const char *key, char *out, size_t size)
{
    if (httpd_query_key_value(body, key, out, size) != ESP_OK)
        return false;
    url_decode(out);
    return true;
}

// Reads a small form body (settings / Wi-Fi) into buf.
static bool read_body(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len >= size)
        return false;
    int got = 0;
    while (got < (int)req->content_len) {
        const int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0)
            return false;
        got += r;
    }
    buf[got] = 0;
    return true;
}

// Appends a JSON string literal (with quotes) to dst.
static int json_str(char *dst, size_t size, const char *s)
{
    size_t o = 0;
    if (o < size) dst[o++] = '"';
    for (; *s && o + 7 < size; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = c;
        } else if (c < 0x20) {
            o += snprintf(dst + o, size - o, "\\u%04x", c);
        } else {
            dst[o++] = c;
        }
    }
    if (o < size) dst[o++] = '"';
    return (int)o;
}

// ---------------------------------------------------------------------------
// Handlers

static esp_err_t handle_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, WEB_PAGE, HTTPD_RESP_USE_STRLEN);
}

// Signal of the joined AP in dBm, 0 when not connected.
static int wifi_rssi()
{
    wifi_ap_record_t ap;
    return ip_str[0] && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

// Everything a client has not seen yet: spectrum rows and text from the given
// sequence numbers on (clamped to what the rings still hold), plus the status.
struct Snapshot {
    uint32_t rseq;                    // sequence number after these rows
    uint32_t tseq[UI_TEXT_COUNT];     // ... and after the text of each channel
    int nrows;
    uint8_t rows[ROWS][UI_BINS];
    char text[UI_TEXT_COUNT][TEXT_RING + 1];
    uint32_t qseq;                    // after these QRSS columns
    int nqrss;
    uint8_t qrss[WEB_QRSS_MAX][UI_QRSS_BINS];
    UiStatus st;
};
EXT_RAM_BSS_ATTR static Snapshot snap;    // used only from the HTTP server task

// A new client (want_row 0) starts with the last 16 rows.
static void take_snapshot(uint32_t want_row, const uint32_t *want_text, uint32_t want_qrss)
{
    snap.nqrss = ui_get_qrss(want_qrss, snap.qrss, WEB_QRSS_MAX, &snap.qseq);
    snap.nrows = ui_get_rows(want_row, snap.rows, want_row ? ROWS : 16, &snap.rseq);
    for (int c = 0; c < UI_TEXT_COUNT; c++)
        ui_get_text((UiTextChannel)c, want_text[c], snap.text[c], &snap.tseq[c]);
    snap.st = ui_get_status();
}

// JSON with status and text; with_rows adds the spectrum rows in base64
// (polling fallback - the WebSocket sends them as a binary frame).
static int format_json(bool with_rows, char *json, size_t cap)
{
    const UiStatus &st = snap.st;
    int n = 0;
    // Text channels (UiTextChannel order): "s" = next sequence numbers, "tx" = new text.
    n += snprintf(json + n, cap - n, "{\"r\":%" PRIu32 ",\"s\":[", snap.rseq);
    for (int c = 0; c < UI_TEXT_COUNT; c++)
        n += snprintf(json + n, cap - n, "%s%" PRIu32, c ? "," : "", snap.tseq[c]);
    n += snprintf(json + n, cap - n, "],\"tx\":[");
    for (int c = 0; c < UI_TEXT_COUNT; c++) {
        if (c)
            n += snprintf(json + n, cap - n, ",");
        n += json_str(json + n, cap - n, snap.text[c]);
    }
    n += snprintf(json + n, cap - n, "]");
    // QRSS columns since the last poll, base64 (UI_QRSS_BINS bytes each).
    n += snprintf(json + n, cap - n, ",\"qs\":%" PRIu32 ",\"qhz\":%d,\"qbin\":%.4f,\"ql\":\"", snap.qseq,
                  g_settings.qrss_hz, qrss_bin_hz());
    if (snap.nqrss > 0 && cap - n > (size_t)(snap.nqrss * UI_QRSS_BINS / 3 * 4 + 16)) {
        base64_encode(&snap.qrss[0][0], snap.nqrss * UI_QRSS_BINS, json + n);
        n += strlen(json + n);
    }
    n += snprintf(json + n, cap - n, "\"");
    if (with_rows) {
        n += snprintf(json + n, cap - n, ",\"rows\":[");
        for (int i = 0; i < snap.nrows; i++) {
            char b64[UI_BINS / 3 * 4 + 8];
            base64_encode(snap.rows[i], UI_BINS, b64);
            n += snprintf(json + n, cap - n, "%s\"%s\"", i ? "," : "", b64);
        }
        n += snprintf(json + n, cap - n, "]");
    }
    n += snprintf(json + n, cap - n, ",\"aprs_n\":%" PRIu32 ",\"aprs_on\":%s", st.aprs_frames,
                  st.aprs_recent ? "true" : "false");
    n += snprintf(json + n, cap - n, ",\"ctcss\":%.1f,\"dcs\":%d,\"dcs_inv\":%s", st.ctcss_hz, st.dcs_code,
                  st.dcs_inv ? "true" : "false");
    n += snprintf(json + n, cap - n, ",\"pocsag\":%s,\"pg_n\":%" PRIu32 ",\"pg_on\":%s",
                  POCSAG_ENABLE ? "true" : "false", st.pocsag_msgs, st.pocsag_recent ? "true" : "false");
    n += snprintf(json + n, cap - n, ",\"psk_hz\":%.1f,\"psk_on\":%s,\"psk_mode\":", st.psk_hz,
                  st.psk_active ? "true" : "false");
    n += json_str(json + n, cap - n, st.psk_mode);
    n += snprintf(json + n, cap - n, ",\"label\":");
    n += json_str(json + n, cap - n, st.label);
    n += snprintf(json + n, cap - n, ",\"ml\":");
    n += json_str(json + n, cap - n, st.ml_label);
    n += snprintf(json + n, cap - n, ",\"ml_p\":%.2f", st.ml_prob);
    n += snprintf(json + n, cap - n,
                  ",\"snr\":%.1f,\"rms\":%.1f,\"pk\":%.1f,\"clip\":%s,\"tone\":%.1f,\"wpm\":%.1f,"
                  "\"cw_auto\":%s,\"cw_manual\":%.1f,\"contrast\":%.2f,"
                  "\"rtty_mark\":%.1f,\"rtty_space\":%.1f,\"rtty_on\":%s,"
                  "\"rtty_baud\":%.2f,\"rtty_pol\":%d,"
                  "\"bin_hz\":%.4f,\"uptime\":%" PRIu32 ",\"load\":%.1f,\"heap\":%u,"
                  "\"rssi\":%d,\"ip\":\"%s\",\"ap\":%s,\"ssid\":",
                  st.snr_db, st.rms_dbfs, st.peak_dbfs, st.clip ? "true" : "false",
                  st.tone_hz, st.wpm,
                  g_settings.cw_auto_tone ? "true" : "false", g_settings.cw_tone_hz,
                  g_settings.cw_min_contrast,
                  st.rtty_mark_hz, st.rtty_space_hz, st.rtty_active ? "true" : "false",
                  g_settings.rtty_baud, g_settings.rtty_polarity,
                  (double)DSP_SAMPLE_RATE / FFT_SIZE, (uint32_t)(esp_timer_get_time() / 1000000),
                  ui_load() * 100.0f, (unsigned)(esp_get_free_heap_size() / 1024),
                  wifi_rssi(), ip_str, ap_active ? "true" : "false");
    n += json_str(json + n, cap - n, g_settings.wifi_ssid);
    n += snprintf(json + n, cap - n, ",\"source\":\"%s\",\"mon\":%s,\"vol\":%d,\"boot\":",
                  AUDIO_SOURCE == AUDIO_SRC_ADC ? "ADC interno" :
                  AUDIO_SOURCE == AUDIO_SRC_ES8311 ? "ES8311" : "PCM1808",
                  AUDIO_MONITOR ? "true" : "false", g_settings.monitor_volume);
    n += json_str(json + n, cap - n, ui_boot_reason());
    n += snprintf(json + n, cap - n,
                  ",\"fax_lpm\":%d,\"fax_ioc\":%d,\"fax_auto\":%s,\"fax_state\":%d,\"fax_lines\":%d",
                  g_settings.fax_lpm, g_settings.fax_ioc, g_settings.fax_auto ? "true" : "false",
                  (int)fax_state(), fax_lines());
    n += snprintf(json + n, cap - n, ",\"sstv_rx\":%s,\"sstv_lines\":%d,\"sstv_mode\":",
                  sstv_receiving() ? "true" : "false", sstv_lines());
    n += json_str(json + n, cap - n, sstv_mode_name());
    const float fs = analyzer_sample_rate();
    n += snprintf(json + n, cap - n, ",\"fs_ppm\":%.0f,\"fs_ok\":%s",
                  fs > 0.0f ? (fs / DSP_SAMPLE_RATE - 1.0f) * 1e6f : 0.0f, fs > 0.0f ? "true" : "false");
    n += snprintf(json + n, cap - n, ",\"gal\":%" PRIu32 ",\"sstv_adjust\":%s", ui_gallery_seq(),
                  g_settings.sstv_adjust ? "true" : "false");
    n += snprintf(json + n, cap - n, ",\"skim\":%d,\"skim_on\":%s", st.skim_channels,
                  g_settings.skim_enabled ? "true" : "false");
    n += snprintf(json + n, cap - n,
                  ",\"mfsk_on\":%s,\"mfsk_snr\":%.1f,\"mfsk_mode\":%d,\"mfsk_tones\":%d,\"mfsk_bw\":%d,\"mfsk_hz\":%d",
                  st.mfsk_active ? "true" : "false", st.mfsk_snr, g_settings.mfsk_mode, g_settings.mfsk_tones,
                  g_settings.mfsk_bw, g_settings.mfsk_hz);
    n += snprintf(json + n, cap - n,
                  ",\"ftx_mode\":%d,\"ftx_time\":%s,\"ftx_n\":%d,\"ftx_ms\":%d,\"ftx_lost\":%d,\"utc\":%lld",
                  g_settings.ftx_mode, ftx_time_ok() ? "true" : "false", ftx_last_count(), ftx_last_ms(),
                  ftx_core_skipped(), (long long)time(nullptr));
    n += snprintf(json + n, cap - n, ",\"boot_bad\":%s,\"resets\":%u}",
                  ui_boot_unexpected() ? "true" : "false", ui_boot_resets());
    return n;
}

EXT_RAM_BSS_ATTR static char json_buf[36 * 1024];    // HTTP server task only (PSRAM)

// GET /api/data?r=<next row>&s=<next char of each text channel, comma-separated>
// Polling fallback for browsers where the WebSocket does not work.
static esp_err_t handle_data(httpd_req_t *req)
{
    uint32_t want_row = 0, want_text[UI_TEXT_COUNT] = {}, want_qrss = 0;
    char q[160], v[120];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "r", v, sizeof(v)) == ESP_OK)
            want_row = strtoul(v, nullptr, 10);
        if (httpd_query_key_value(q, "q", v, sizeof(v)) == ESP_OK)
            want_qrss = strtoul(v, nullptr, 10);
        if (httpd_query_key_value(q, "s", v, sizeof(v)) == ESP_OK) {
            const char *p = v;
            for (int c = 0; c < UI_TEXT_COUNT && *p; c++) {
                char *end;
                want_text[c] = strtoul(p, &end, 10);
                p = *end == ',' ? end + 1 : end;
            }
        }
    }
    const int64_t t0 = esp_timer_get_time();
    take_snapshot(want_row, want_text, want_qrss);
    const int n = format_json(true, json_buf, sizeof(json_buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, json_buf, n);
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (err != ESP_OK || ms > 500)
        ESP_LOGW(TAG, "/api/data: %d bytes em %lld ms (%s)", n, ms, esp_err_to_name(err));
    return err;
}

// GET /api/img?i=<image id>&l=<next line>
// Binary, little endian: u32 id, u32 first line sent, u32 lines in the image,
// u16 width, u8 channels, u8 receiving, f32 aspect, char title[32], then the
// lines from `first` on (at most IMG_MAX_BYTES). A different id restarts from
// the oldest line still held.
// Small replies: one HTTP task serves everything, and a long send (hundreds of
// KB on a weak link) starved the WebSocket until the page dropped it.
#define IMG_MAX_BYTES (32 * 1024)

static esp_err_t handle_img(httpd_req_t *req)
{
    uint32_t want_id = 0, want_line = 0;
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "i", v, sizeof(v)) == ESP_OK)
            want_id = strtoul(v, nullptr, 10);
        if (httpd_query_key_value(q, "l", v, sizeof(v)) == ESP_OK)
            want_line = strtoul(v, nullptr, 10);
    }
    uint8_t head[20 + IMG_TITLE] = {};
    const UiImageInfo info = ui_get_image_info();
    const uint32_t id = info.id, total = info.lines;
    const size_t bytes = info.width * info.channels;
    uint32_t first = want_id == id ? want_line : 0;
    if (first > total)
        first = 0;
    if (first < info.first)
        first = info.first;
    memcpy(head, &id, 4);
    memcpy(head + 4, &first, 4);
    memcpy(head + 8, &total, 4);
    memcpy(head + 12, &info.width, 2);
    head[14] = info.channels;
    head[15] = info.active;
    memcpy(head + 16, &info.aspect, 4);
    memcpy(head + 20, info.title, IMG_TITLE);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)head, sizeof(head));
    // A few lines at a time: copied under the hub lock, sent without it.
    EXT_RAM_BSS_ATTR static uint8_t buf[2 * UI_IMG_MAX_LINE];    // HTTP server task only
    const uint32_t max_lines = bytes ? (IMG_MAX_BYTES / bytes > 0 ? IMG_MAX_BYTES / bytes : 1) : 0;
    const uint32_t last = total - first > max_lines ? first + max_lines : total;
    const uint32_t per_send = bytes ? sizeof(buf) / bytes : 0;
    for (uint32_t k = first; k < last && err == ESP_OK;) {
        const uint32_t n = last - k < per_send ? last - k : per_send;
        if (!ui_get_image_lines(id, k, n, buf))
            break;    // overwritten meanwhile: the client asks again
        err = httpd_resp_send_chunk(req, (const char *)buf, n * bytes);
        k += n;
    }
    if (err == ESP_OK)
        err = httpd_resp_send_chunk(req, nullptr, 0);
    return err;
}

// GET /api/ftx?s=<next message>
// {"s":<next>,"m":[[slot start (UTC s), snr, dt, Hz, "text"], ...]}
static esp_err_t handle_ftx(httpd_req_t *req)
{
    uint32_t want = 0;
    char q[32], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "s", v, sizeof(v)) == ESP_OK)
        want = strtoul(v, nullptr, 10);
    EXT_RAM_BSS_ATTR static FtxMessage msgs[FTX_RING];    // HTTP server task only
    uint32_t seq;
    const int n = ui_get_ftx(want, msgs, &seq);

    EXT_RAM_BSS_ATTR static char json[FTX_RING * 96 + 32];
    int o = snprintf(json, sizeof(json), "{\"s\":%" PRIu32 ",\"m\":[", seq);
    for (int i = 0; i < n; i++) {
        const FtxMessage &m = msgs[i];
        o += snprintf(json + o, sizeof(json) - o, "%s[%lld,%.0f,%.1f,%.0f,", i ? "," : "",
                      (long long)m.slot_start, m.snr_db, m.dt, m.freq_hz);
        o += json_str(json + o, sizeof(json) - o, m.text);
        o += snprintf(json + o, sizeof(json) - o, "]");
    }
    o += snprintf(json + o, sizeof(json) - o, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, o);
}

// GET /api/gallery -> {"seq":<n>,"imgs":[{"n":..,"title":..,"w":..,"h":..,"utc":..}, ...]}
static esp_err_t handle_gallery(httpd_req_t *req)
{
    char json[UI_GALLERY * 128 + 32];
    UiGalleryEntry list[UI_GALLERY];
    const int count = ui_gallery_list(list);
    int o = snprintf(json, sizeof(json), "{\"seq\":%" PRIu32 ",\"imgs\":[", ui_gallery_seq());
    for (int i = 0; i < count; i++) {
        const UiGalleryEntry &g = list[i];
        o += snprintf(json + o, sizeof(json) - o, "%s{\"n\":%" PRIu32 ",\"w\":%u,\"h\":%u,\"utc\":%lld,\"title\":",
                      i ? "," : "", g.n, g.w, g.h, (long long)g.utc);
        o += json_str(json + o, sizeof(json) - o, g.title);
        o += snprintf(json + o, sizeof(json) - o, "}");
    }
    o += snprintf(json + o, sizeof(json) - o, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, o);
}

// GET /api/gallery_img?n=<n>&o=<offset> -> up to IMG_MAX_BYTES of the RGB
// pixels (w*h*3 bytes, size from /api/gallery) from byte `offset` on.
static esp_err_t handle_gallery_img(httpd_req_t *req)
{
    uint32_t want = 0;
    size_t off = 0;
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK)
            want = strtoul(v, nullptr, 10);
        if (httpd_query_key_value(q, "o", v, sizeof(v)) == ESP_OK)
            off = strtoul(v, nullptr, 10);
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // In chunks copied under the hub lock: the slot may be reused meanwhile.
    EXT_RAM_BSS_ATTR static uint8_t buf[8192];    // HTTP server task only
    const size_t start = off;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && off - start < IMG_MAX_BYTES) {
        const size_t room = start + IMG_MAX_BYTES - off;
        const int n = ui_gallery_read(want, off, buf, room < sizeof(buf) ? room : sizeof(buf));
        if (n < 0)
            return off > start ? ESP_FAIL : httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "sem imagem");
        if (!n)
            break;
        err = httpd_resp_send_chunk(req, (const char *)buf, n);
        off += n;
    }
    if (err == ESP_OK)
        err = httpd_resp_send_chunk(req, nullptr, 0);
    return err;
}

// ---------------------------------------------------------------------------
// WebSocket /ws: the server pushes every WS_PUSH_MS a binary frame with the new
// spectrum rows ([1][n][n x UI_BINS]) and a text frame with the status JSON.
// Client bookkeeping and sends all run in the HTTP server task (handlers and
// httpd_queue_work), so a socket is never written from two tasks.

#define WS_MAX_CLIENTS 4
#define WS_PUSH_MS     250    // batches ~3 rows: few, larger Wi-Fi bursts
#define WS_STATUS_US   1000000

// ESP-IDF 6 does not call the URI handler for the WebSocket handshake, so
// clients are discovered from the server's socket list on every push.
struct WsClient {
    bool used;
    int fd;
    uint32_t row;             // next sequence numbers this client needs: rows,
    uint32_t text[UI_TEXT_COUNT];    // text of each channel
    uint32_t qrss;            // QRSS columns
    int64_t last_status_us;   // status JSON goes out once a second (or with new text)
};
static WsClient ws_clients[WS_MAX_CLIENTS];
static volatile bool ws_push_pending = false;
static httpd_handle_t server = nullptr;
EXT_RAM_BSS_ATTR static uint8_t ws_bin[2 + ROWS * UI_BINS];

// Clients never send data; read and discard whatever arrives.
static esp_err_t handle_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
        return ESP_OK;    // handshake (not called by ESP-IDF 6, kept for older versions)
    httpd_ws_frame_t f = {};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK || f.len == 0)
        return err;
    uint8_t discard[64];
    while (f.len > 0 && err == ESP_OK) {
        f.payload = discard;
        const size_t chunk = f.len < sizeof(discard) ? f.len : sizeof(discard);
        err = httpd_ws_recv_frame(req, &f, chunk);
        f.len -= chunk;
    }
    return err;
}

static uint32_t ws_frames_sent = 0;

static void ws_add_client(int fd, const char *how)
{
    for (const WsClient &c : ws_clients) {
        if (c.used && c.fd == fd)
            return;
    }
    for (WsClient &c : ws_clients) {
        if (!c.used) {
            c = {};    // sequence numbers 0 = start with the recent backlog
            c.used = true;
            c.fd = fd;
            ESP_LOGW(TAG, "WS: cliente fd %d ligado (%s)", fd, how);
            return;
        }
    }
    ESP_LOGW(TAG, "WS: sem lugar para o cliente fd %d", fd);
}

// Called by the server right after the WebSocket handshake.
static esp_err_t ws_on_handshake(httpd_req_t *req)
{
    ws_add_client(httpd_req_to_sockfd(req), "handshake");
    return ESP_OK;
}

// Syncs ws_clients with the WebSocket sessions the server has open.
static void ws_refresh_clients()
{
    for (WsClient &c : ws_clients) {
        if (!c.used)
            continue;
        const httpd_ws_client_info_t info = httpd_ws_get_fd_info(server, c.fd);
        if (info != HTTPD_WS_CLIENT_WEBSOCKET) {
            c.used = false;    // closed (the fd may now belong to a plain HTTP request)
            ESP_LOGW(TAG, "WS: cliente fd %d saiu (%s)", c.fd,
                     info == HTTPD_WS_CLIENT_INVALID ? "sessao fechada" : "ja nao e WebSocket");
        }
    }
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    size_t nfds = sizeof(fds) / sizeof(fds[0]);
    if (httpd_get_client_list(server, &nfds, fds) != ESP_OK)
        return;
    int nws = 0;
    for (size_t i = 0; i < nfds; i++) {
        if (httpd_ws_get_fd_info(server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET)
            continue;
        nws++;
        ws_add_client(fds[i], "lista");
    }

    // Heartbeat every ~10 s while anything is connected.
    static int beat = 0;
    if (nfds > 0 && ++beat >= 10000 / WS_PUSH_MS) {
        beat = 0;
        int nclients = 0;
        for (const WsClient &c : ws_clients)
            nclients += c.used;
        ESP_LOGW(TAG, "WS diag: %u ligacoes (%d WebSocket), %d clientes, %" PRIu32 " envios",
                 (unsigned)nfds, nws, nclients, ws_frames_sent);
    }
}

static void ws_push_work(void *)
{
    ws_refresh_clients();
    for (WsClient &c : ws_clients) {
        if (!c.used)
            continue;
        take_snapshot(c.row, c.text, c.qrss);

        httpd_ws_frame_t f = {};
        esp_err_t err = ESP_OK;
        const int64_t t_send = esp_timer_get_time();
        if (snap.nrows > 0) {
            ws_bin[0] = 1;
            ws_bin[1] = (uint8_t)snap.nrows;
            memcpy(ws_bin + 2, snap.rows, snap.nrows * UI_BINS);
            f.type = HTTPD_WS_TYPE_BINARY;
            f.payload = ws_bin;
            f.len = 2 + snap.nrows * UI_BINS;
            err = httpd_ws_send_frame_async(server, c.fd, &f);
        }
        const int64_t now = esp_timer_get_time();
        bool new_text = snap.qseq != c.qrss;
        for (int k = 0; k < UI_TEXT_COUNT; k++)
            new_text |= snap.tseq[k] != c.text[k];
        const bool send_status = new_text || now - c.last_status_us >= WS_STATUS_US;
        if (err == ESP_OK && send_status) {
            f.type = HTTPD_WS_TYPE_TEXT;
            f.payload = (uint8_t *)json_buf;
            f.len = format_json(false, json_buf, sizeof(json_buf));
            err = httpd_ws_send_frame_async(server, c.fd, &f);
            c.last_status_us = now;
        }
        const int64_t send_ms = (esp_timer_get_time() - t_send) / 1000;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WS: envio para fd %d falhou apos %lld ms: %s (errno %d), a fechar",
                     c.fd, send_ms, esp_err_to_name(err), errno);
            httpd_sess_trigger_close(server, c.fd);
            c.used = false;
            continue;
        }
        if (send_ms > 500)
            ESP_LOGW(TAG, "WS: envio para fd %d lento (%lld ms)", c.fd, send_ms);
        ws_frames_sent++;
        c.row = snap.rseq;
        if (send_status) {
            memcpy(c.text, snap.tseq, sizeof(c.text));
            c.qrss = snap.qseq;
        }
    }
    ws_push_pending = false;
}

static void ws_push_tick(void *)
{
    if (!ws_push_pending && server) {
        ws_push_pending = true;
        if (httpd_queue_work(server, ws_push_work, nullptr) != ESP_OK)
            ws_push_pending = false;
    }
}

// POST /api/config  cw_auto=0|1 & cw_tone=Hz & contrast=x & rtty_baud=b & rtty_pol=0..2
static esp_err_t handle_config(httpd_req_t *req)
{
    char body[256], v[32];
    if (!read_body(req, body, sizeof(body)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pedido invalido");

    if (form_value(body, "cw_auto", v, sizeof(v)))
        g_settings.cw_auto_tone = atoi(v) != 0;
    if (form_value(body, "cw_tone", v, sizeof(v))) {
        const float hz = strtof(v, nullptr);
        if (hz >= SPECTRUM_MIN_HZ && hz <= SPECTRUM_MAX_HZ)
            g_settings.cw_tone_hz = hz;
    }
    if (form_value(body, "contrast", v, sizeof(v))) {
        const float c = strtof(v, nullptr);
        if (c >= 1.5f && c <= 20.0f)
            g_settings.cw_min_contrast = c;
    }
    if (form_value(body, "rtty_baud", v, sizeof(v))) {
        const float b = strtof(v, nullptr);
        if (b >= 30.0f && b <= 300.0f)
            g_settings.rtty_baud = b;    // applied by the analyzer on its next report
    }
    if (form_value(body, "rtty_pol", v, sizeof(v))) {
        const int p = atoi(v);
        if (p >= 0 && p <= 2)
            g_settings.rtty_polarity = p;
    }
    if (form_value(body, "volume", v, sizeof(v))) {
        const int vol = atoi(v);
        if (vol >= 0 && vol <= 100)
            g_settings.monitor_volume = vol;
    }
    if (form_value(body, "fax_lpm", v, sizeof(v))) {
        const int l = atoi(v);
        if (l == 60 || l == 90 || l == 120 || l == 240)
            g_settings.fax_lpm = l;
    }
    if (form_value(body, "fax_ioc", v, sizeof(v))) {
        const int i = atoi(v);
        if (i == 288 || i == 576)
            g_settings.fax_ioc = i;
    }
    if (form_value(body, "fax_auto", v, sizeof(v)))
        g_settings.fax_auto = atoi(v) != 0;
    if (form_value(body, "sstv_adjust", v, sizeof(v)))
        g_settings.sstv_adjust = atoi(v) != 0;
    if (form_value(body, "skim_on", v, sizeof(v)))
        g_settings.skim_enabled = atoi(v) != 0;
    if (form_value(body, "mfsk_mode", v, sizeof(v))) {
        const int m = atoi(v);
        if (m >= MFSK_OFF && m <= MFSK_CONTESTIA)
            g_settings.mfsk_mode = m;
    }
    if (form_value(body, "mfsk_tones", v, sizeof(v)) || form_value(body, "mfsk_bw", v, sizeof(v))) {
        char tv[12] = "", bv[12] = "";
        const int t = form_value(body, "mfsk_tones", tv, sizeof(tv)) ? atoi(tv) : g_settings.mfsk_tones;
        const int b = form_value(body, "mfsk_bw", bv, sizeof(bv)) ? atoi(bv) : g_settings.mfsk_bw;
        if (mfsk_valid(t, b)) {
            g_settings.mfsk_tones = t;
            g_settings.mfsk_bw = b;
        }
    }
    if (form_value(body, "mfsk_hz", v, sizeof(v))) {
        const int hz = atoi(v);
        if (hz >= 300 && hz <= 3000)
            g_settings.mfsk_hz = hz;
    }
    if (form_value(body, "qrss_hz", v, sizeof(v))) {
        const int hz = atoi(v);
        if (hz >= 300 && hz <= 3000)
            g_settings.qrss_hz = hz;
    }
    if (form_value(body, "ftx_mode", v, sizeof(v))) {
        const int m = atoi(v);
        if (m >= FTX_OFF && m < FTX_PROTOCOLS)
            g_settings.ftx_mode = m;
    }
    // The page's clock (UTC ms) when NTP has not set ours (access point
    // without internet): FT8/FT4 need the time to within ~1 s.
    if (form_value(body, "utc_ms", v, sizeof(v)) && !ftx_time_ok()) {
        const long long ms = strtoll(v, nullptr, 10);
        struct timeval tv = { (time_t)(ms / 1000), (suseconds_t)(ms % 1000 * 1000) };
        settimeofday(&tv, nullptr);
        ESP_LOGW(TAG, "hora UTC recebida da pagina");
    }
    // One-off actions, not settings.
    if (form_value(body, "fax_shift", v, sizeof(v)))
        fax_request_shift(strtof(v, nullptr));
    if (form_value(body, "fax_start", v, sizeof(v)))
        fax_request_start();
    if (form_value(body, "img_stop", v, sizeof(v))) {
        fax_request_stop();
        sstv_request_stop();
    }
    settings_apply();
    settings_save();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void restart_task(void *)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

// POST /api/wifi  ssid=... & pass=...   (saves and restarts)
static esp_err_t handle_wifi(httpd_req_t *req)
{
    char body[256];
    if (!read_body(req, body, sizeof(body)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pedido invalido");
    char ssid[sizeof(g_settings.wifi_ssid)] = "", pass[sizeof(g_settings.wifi_pass)] = "";
    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || !ssid[0])
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta o SSID");
    form_value(body, "pass", pass, sizeof(pass));
    strlcpy(g_settings.wifi_ssid, ssid, sizeof(g_settings.wifi_ssid));
    strlcpy(g_settings.wifi_pass, pass, sizeof(g_settings.wifi_pass));
    settings_save();
    ESP_LOGW(TAG, "nova rede Wi-Fi \"%s\", a reiniciar", ssid);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(restart_task, "restart", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

static void register_uri(const char *uri, httpd_method_t method,
                         esp_err_t (*handler)(httpd_req_t *), bool websocket = false)
{
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = handler;
    u.is_websocket = websocket;
    if (websocket)
        u.ws_post_handshake_cb = ws_on_handshake;
    httpd_register_uri_handler(server, &u);
}

static void http_start()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;    // the internal RAM is needed by the audio DMA and Wi-Fi
    cfg.core_id = 0;    // keep off the audio/analysis core
    // Browsers keep connections open; without LRU purge the few sockets run
    // out after a while and requests hang (the page froze after minutes).
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 12;
    cfg.max_open_sockets = 6;
    cfg.recv_wait_timeout = 5;
    // One task serves every client: a send to a dead peer blocked it for the
    // whole timeout, stalling the page. Short send timeout + TCP keepalive
    // (dead connections closed after ~10 s).
    cfg.send_wait_timeout = 2;
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 5;
    cfg.keep_alive_interval = 2;
    cfg.keep_alive_count = 3;
    const esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s (RAM interna livre %u B); sem pagina web",
                 esp_err_to_name(err), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return;
    }

    register_uri("/",           HTTP_GET,  handle_page);
    register_uri("/api/data",   HTTP_GET,  handle_data);
    register_uri("/api/config", HTTP_POST, handle_config);
    register_uri("/api/wifi",   HTTP_POST, handle_wifi);
    register_uri("/api/img",    HTTP_GET,  handle_img);
    register_uri("/api/ftx",    HTTP_GET,  handle_ftx);
    register_uri("/api/gallery", HTTP_GET, handle_gallery);
    register_uri("/api/gallery_img", HTTP_GET, handle_gallery_img);
    register_uri("/ws",         HTTP_GET,  handle_ws, true);

    esp_timer_create_args_t targs = {};
    targs.callback = ws_push_tick;
    targs.name = "ws_push";
    esp_timer_handle_t push_timer;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &push_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(push_timer, WS_PUSH_MS * 1000ULL));
}

// Joining the network waits up to STA_TIMEOUT_MS, so it runs in its own task.
static void web_task(void *)
{
    if (wifi_start())
        http_start();
    vTaskDelete(nullptr);
}

static volatile bool web_started = false;

void web_start()
{
    web_started = true;
    xTaskCreatePinnedToCore(web_task, "web_start", 4096, nullptr, 5, nullptr, 0);
}

WebNetInfo web_net_info()
{
    WebNetInfo info = {};
    info.started = web_started;
    info.connected = ip_str[0] != 0;
    info.ap = ap_active;
    info.fail_reason = fail_reason;
    info.rssi = web_started ? wifi_rssi() : 0;
    strlcpy(info.ip, ip_str, sizeof(info.ip));
    return info;
}

bool web_scan_start()
{
    if (!wifi_ready || scanning)
        return false;
    scanning = true;
    scan_state = 1;
    if (!ip_str[0])
        esp_wifi_disconnect();    // a scan cannot run while a join is in progress
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
        scanning = false;
        scan_state = 0;
        return false;
    }
    return true;
}

int web_scan_result(WebScanEntry *out, int max, int *n)
{
    *n = 0;
    if (scan_state != 2)
        return scan_state;
    *n = scan_count < max ? scan_count : max;
    memcpy(out, scan_list, *n * sizeof(WebScanEntry));
    return 2;
}

bool web_connect(const char *ssid, const char *pass)
{
    strlcpy(g_settings.wifi_ssid, ssid, sizeof(g_settings.wifi_ssid));
    strlcpy(g_settings.wifi_pass, pass, sizeof(g_settings.wifi_pass));
    settings_save();
    if (!wifi_ready)
        return false;
    ESP_LOGW(TAG, "nova rede Wi-Fi \"%s\"", ssid);
    fail_reason = 0;
    ip_str[0] = 0;
    set_sta_config();
    esp_wifi_disconnect();
    esp_wifi_connect();
    return true;
}

#else  // !WEB_UI

void web_start() {}
bool web_scan_start() { return false; }
int web_scan_result(WebScanEntry *, int, int *n) { *n = 0; return 0; }
bool web_connect(const char *, const char *) { return false; }

WebNetInfo web_net_info()
{
    return {};
}

#endif // WEB_UI
