#include "web_ui.h"

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

#include "config.h"
#include "settings.h"
#include "cw_decoder.h"

static const char *TAG = "WEB";

#include "web_page.inc"    // const char WEB_PAGE[]

// ---------------------------------------------------------------------------
// Shared state (analysis task writes, HTTP task reads), guarded by `lock`.

#define ROWS 64                 // ~5 s of spectrum rows at ~12 rows/s
#define TEXT_RING 2048

static SemaphoreHandle_t lock;

static uint8_t rows[ROWS][WEB_BINS];
static uint32_t row_seq = 0;    // number of rows ever pushed

struct TextRing {
    char buf[TEXT_RING];
    uint32_t seq = 0;           // number of characters ever pushed
};
static TextRing texts[2];       // WebTextChannel: CW, RTTY

static WebStatus status;
static volatile float analysis_load = 0.0f;
static const char *boot_reason = "";
static bool boot_unexpected = false;
static unsigned boot_resets = 0;

void web_set_boot_info(const char *reason, bool unexpected, unsigned resets)
{
    boot_reason = reason;
    boot_unexpected = unexpected;
    boot_resets = resets;
}

void web_set_load(float fraction)
{
    analysis_load = fraction;
}

void web_push_spectrum(const uint8_t *row)
{
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(rows[row_seq % ROWS], row, WEB_BINS);
    row_seq++;
    xSemaphoreGive(lock);
}

void web_push_status(const WebStatus &st)
{
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    status = st;
    xSemaphoreGive(lock);
}

void web_push_text(WebTextChannel ch, const char *text)
{
    if (!lock || !text[0])
        return;
    TextRing &t = texts[ch];
    xSemaphoreTake(lock, portMAX_DELAY);
    for (const char *p = text; *p; p++)
        t.buf[t.seq++ % TEXT_RING] = *p;
    xSemaphoreGive(lock);
}

// Copies the characters of ring `t` from sequence `from` on (clamped to what
// the ring still holds; a new client gets the last 512). Returns the count.
static int snapshot_text(const TextRing &t, uint32_t from, char *out)
{
    if (from > t.seq || t.seq - from > TEXT_RING)
        from = t.seq > 512 ? t.seq - 512 : 0;
    const int n = t.seq - from;
    for (int i = 0; i < n; i++)
        out[i] = t.buf[(from + i) % TEXT_RING];
    out[n] = 0;
    return n;
}

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

static void retry_connect(void *)
{
    esp_wifi_connect();
}
static char ip_str[16] = "";

static void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_settings.wifi_ssid[0])
            esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Wi-Fi desligado (motivo %d, RSSI %d dBm)", ev->reason, ev->rssi);
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        ip_str[0] = 0;
        // Keep retrying in the background, without blocking the event loop.
        if (g_settings.wifi_ssid[0] && retry_timer)
            esp_timer_start_once(retry_timer, STA_RETRY_MS * 1000ULL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGW(TAG, "ligado a \"%s\": http://%s/", g_settings.wifi_ssid, ip_str);
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
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

static void wifi_start()
{
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, nullptr));

    esp_timer_create_args_t targs = {};
    targs.callback = retry_connect;
    targs.name = "wifi_retry";
    ESP_ERROR_CHECK(esp_timer_create(&targs, &retry_timer));

    wifi_config_t sta = {};
    strlcpy((char *)sta.sta.ssid, g_settings.wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, g_settings.wifi_pass, sizeof(sta.sta.password));
    // Scan every channel and join the strongest AP with this SSID (repeaters).
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Radio always on (no modem-sleep bursts on the ADC, steadier latency)
    // and at reduced power.
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);

    if (!g_settings.wifi_ssid[0]) {
        ESP_LOGW(TAG, "sem rede Wi-Fi configurada");
        start_access_point();
        return;
    }
    ESP_LOGI(TAG, "a ligar a \"%s\"...", g_settings.wifi_ssid);
    const EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                                 pdMS_TO_TICKS(STA_TIMEOUT_MS));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "nao liga a \"%s\" (continua a tentar)", g_settings.wifi_ssid);
        start_access_point();
    }
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
    uint32_t rseq, tseq, useq;    // sequence numbers after these rows/text
    int nrows;
    uint8_t rows[ROWS][WEB_BINS];
    char text[2][TEXT_RING + 1];
    WebStatus st;
};
static Snapshot snap;    // used only from the HTTP server task

static void take_snapshot(uint32_t want_row, uint32_t want_t, uint32_t want_u)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    snap.rseq = row_seq;
    snap.tseq = texts[WEB_TEXT_CW].seq;
    snap.useq = texts[WEB_TEXT_RTTY].seq;
    uint32_t first_row = want_row;
    if (first_row > snap.rseq || snap.rseq - first_row > ROWS)
        first_row = snap.rseq > 16 ? snap.rseq - 16 : 0;    // new client or fell behind
    snap.nrows = snap.rseq - first_row;
    for (int i = 0; i < snap.nrows; i++)
        memcpy(snap.rows[i], rows[(first_row + i) % ROWS], WEB_BINS);
    snapshot_text(texts[WEB_TEXT_CW], want_t, snap.text[WEB_TEXT_CW]);
    snapshot_text(texts[WEB_TEXT_RTTY], want_u, snap.text[WEB_TEXT_RTTY]);
    snap.st = status;
    xSemaphoreGive(lock);
}

// JSON with status and text; with_rows adds the spectrum rows in base64
// (polling fallback - the WebSocket sends them as a binary frame).
static int format_json(bool with_rows, char *json, size_t cap)
{
    const WebStatus &st = snap.st;
    int n = 0;
    n += snprintf(json + n, cap - n, "{\"r\":%" PRIu32 ",\"t\":%" PRIu32 ",\"u\":%" PRIu32,
                  snap.rseq, snap.tseq, snap.useq);
    if (with_rows) {
        n += snprintf(json + n, cap - n, ",\"rows\":[");
        for (int i = 0; i < snap.nrows; i++) {
            char b64[WEB_BINS / 3 * 4 + 8];
            base64_encode(snap.rows[i], WEB_BINS, b64);
            n += snprintf(json + n, cap - n, "%s\"%s\"", i ? "," : "", b64);
        }
        n += snprintf(json + n, cap - n, "]");
    }
    n += snprintf(json + n, cap - n, ",\"text\":");
    n += json_str(json + n, cap - n, snap.text[WEB_TEXT_CW]);
    n += snprintf(json + n, cap - n, ",\"rtext\":");
    n += json_str(json + n, cap - n, snap.text[WEB_TEXT_RTTY]);
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
                  analysis_load * 100.0f, (unsigned)(esp_get_free_heap_size() / 1024),
                  wifi_rssi(), ip_str, ap_active ? "true" : "false");
    n += json_str(json + n, cap - n, g_settings.wifi_ssid);
    n += snprintf(json + n, cap - n, ",\"source\":\"%s\",\"boot\":",
                  AUDIO_SOURCE == AUDIO_SRC_ADC ? "ADC interno" : "PCM1808");
    n += json_str(json + n, cap - n, boot_reason);
    n += snprintf(json + n, cap - n, ",\"boot_bad\":%s,\"resets\":%u}",
                  boot_unexpected ? "true" : "false", boot_resets);
    return n;
}

static char json_buf[28 * 1024];    // HTTP server task only

// GET /api/data?r=<next row>&t=<next CW char>&u=<next RTTY char>
// Polling fallback for browsers where the WebSocket does not work.
static esp_err_t handle_data(httpd_req_t *req)
{
    uint32_t want_row = 0, want_t = 0, want_u = 0;
    char q[80], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "r", v, sizeof(v)) == ESP_OK)
            want_row = strtoul(v, nullptr, 10);
        if (httpd_query_key_value(q, "t", v, sizeof(v)) == ESP_OK)
            want_t = strtoul(v, nullptr, 10);
        if (httpd_query_key_value(q, "u", v, sizeof(v)) == ESP_OK)
            want_u = strtoul(v, nullptr, 10);
    }
    const int64_t t0 = esp_timer_get_time();
    take_snapshot(want_row, want_t, want_u);
    const int n = format_json(true, json_buf, sizeof(json_buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, json_buf, n);
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (err != ESP_OK || ms > 500)
        ESP_LOGW(TAG, "/api/data: %d bytes em %lld ms (%s)", n, ms, esp_err_to_name(err));
    return err;
}

// ---------------------------------------------------------------------------
// WebSocket /ws: the server pushes every WS_PUSH_MS a binary frame with the new
// spectrum rows ([1][n][n x WEB_BINS]) and a text frame with the status JSON.
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
    uint32_t row, t, u;       // next sequence numbers this client needs
    int64_t last_status_us;   // status JSON goes out once a second (or with new text)
};
static WsClient ws_clients[WS_MAX_CLIENTS];
static volatile bool ws_push_pending = false;
static httpd_handle_t server = nullptr;
static uint8_t ws_bin[2 + ROWS * WEB_BINS];

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
            c = { true, fd, 0, 0, 0, 0 };    // 0 = start with the recent backlog
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
        take_snapshot(c.row, c.t, c.u);

        httpd_ws_frame_t f = {};
        esp_err_t err = ESP_OK;
        const int64_t t_send = esp_timer_get_time();
        if (snap.nrows > 0) {
            ws_bin[0] = 1;
            ws_bin[1] = (uint8_t)snap.nrows;
            memcpy(ws_bin + 2, snap.rows, snap.nrows * WEB_BINS);
            f.type = HTTPD_WS_TYPE_BINARY;
            f.payload = ws_bin;
            f.len = 2 + snap.nrows * WEB_BINS;
            err = httpd_ws_send_frame_async(server, c.fd, &f);
        }
        const int64_t now = esp_timer_get_time();
        const bool new_text = snap.tseq != c.t || snap.useq != c.u;
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
            c.t = snap.tseq;
            c.u = snap.useq;
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
    cw_set_min_contrast(g_settings.cw_min_contrast);
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
    cfg.core_id = 0;    // keep off the audio/analysis core
    // Browsers keep connections open; without LRU purge the few sockets run
    // out after a while and requests hang (the page froze after minutes).
    cfg.lru_purge_enable = true;
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
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    register_uri("/",           HTTP_GET,  handle_page);
    register_uri("/api/data",   HTTP_GET,  handle_data);
    register_uri("/api/config", HTTP_POST, handle_config);
    register_uri("/api/wifi",   HTTP_POST, handle_wifi);
    register_uri("/ws",         HTTP_GET,  handle_ws, true);

    esp_timer_create_args_t targs = {};
    targs.callback = ws_push_tick;
    targs.name = "ws_push";
    esp_timer_handle_t push_timer;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &push_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(push_timer, WS_PUSH_MS * 1000ULL));
}

void web_start()
{
    lock = xSemaphoreCreateMutex();
    cw_set_min_contrast(g_settings.cw_min_contrast);
    wifi_start();
    http_start();
}
