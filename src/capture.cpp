#include "capture.h"

#include <stdio.h>
#include <stdint.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "config.h"

#if CAPTURE_SECONDS > 0

static const char *TAG = "CAPTURE";

constexpr int CAP_SAMPLES = DSP_SAMPLE_RATE * CAPTURE_SECONDS;
constexpr int SAMPLES_PER_LINE = 60;    // 120 bytes -> 160 base64 characters

static int16_t *buf = nullptr;
static int pos = 0;
static enum { IDLE, RECORDING, DONE } state = IDLE;

static void base64_line(const uint8_t *data, int len)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char out[(SAMPLES_PER_LINE * 2 + 2) / 3 * 4 + 1];
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        const uint32_t v = (uint32_t)data[i] << 16 |
                           (i + 1 < len ? (uint32_t)data[i + 1] << 8 : 0) |
                           (i + 2 < len ? (uint32_t)data[i + 2] : 0);
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[o] = 0;
    printf("CAP:%s\n", out);
}

static void dump()
{
    printf("\nCAPTURE_BEGIN rate=%d samples=%d format=s16le\n", DSP_SAMPLE_RATE, CAP_SAMPLES);
    for (int i = 0; i < CAP_SAMPLES; i += SAMPLES_PER_LINE) {
        const int n = CAP_SAMPLES - i < SAMPLES_PER_LINE ? CAP_SAMPLES - i : SAMPLES_PER_LINE;
        base64_line((const uint8_t *)&buf[i], n * 2);    // ESP32 is little-endian
        vTaskDelay(1);    // let the idle task run: the task watchdog fired mid-dump
    }
    printf("CAPTURE_END\n\n");
    fflush(stdout);
}

void capture_init()
{
    buf = (int16_t *)heap_caps_malloc(CAP_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "sem PSRAM para %d s de captura", CAPTURE_SECONDS);
        state = DONE;
        return;
    }
    ESP_LOGI(TAG, "captura de %d s pronta: comeca quando o CW sintonizar", CAPTURE_SECONDS);
}

void capture_push(const float *x, int n, bool start)
{
    if (state == DONE)
        return;
    if (state == IDLE) {
        if (!start)
            return;
        state = RECORDING;
        ESP_LOGW(TAG, "a gravar %d s...", CAPTURE_SECONDS);
    }

    for (int i = 0; i < n && pos < CAP_SAMPLES; i++) {
        float v = x[i] * 32767.0f;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        buf[pos++] = (int16_t)lrintf(v);
    }

    if (pos >= CAP_SAMPLES) {
        ESP_LOGW(TAG, "a enviar a gravacao (~%d s a 115200 baud) - espera pelo CAPTURE_END...",
                 CAP_SAMPLES * 2 * 4 / 3 / 11000 + CAP_SAMPLES / SAMPLES_PER_LINE / 1000);
        dump();
        heap_caps_free(buf);
        buf = nullptr;
        state = DONE;
    }
}

#else

void capture_init() {}
void capture_push(const float *, int, bool) {}

#endif
