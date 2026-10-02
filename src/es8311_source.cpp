#include "config.h"

#if AUDIO_SOURCE == AUDIO_SRC_ES8311

#include "audio_source.h"
#include "es8311.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "ES8311";

#define ES_FRAMES           256
// ES8311 settling plus its ADC high-pass.
#define ES_STARTUP_DISCARD_MS   300

void audio_source_init()
{
    if (!es8311_start())
        ESP_LOGE(TAG, "sem codec: a entrada de audio vai ficar a zero");

    static int32_t discard[ES_FRAMES * 2];
    size_t bytes;
    const int64_t frames = (int64_t)AUDIO_SAMPLE_RATE * ES_STARTUP_DISCARD_MS / 1000;
    for (int64_t got = 0; got < frames; got += bytes / (2 * sizeof(int32_t)))
        i2s_channel_read(es8311_rx(), discard, sizeof(discard), &bytes, portMAX_DELAY);
}

void audio_source_log() {}

size_t audio_source_read(int32_t *out, size_t max)
{
    static int32_t frames[ES_FRAMES * 2];
    size_t bytes = 0;

    esp_err_t err = i2s_channel_read(es8311_rx(), frames, sizeof(frames), &bytes, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S read: %s", esp_err_to_name(err));
        return 0;
    }

    size_t n = bytes / (2 * sizeof(int32_t));
    if (n > max)
        n = max;
    for (size_t i = 0; i < n; i++)
        out[i] = frames[2 * i + ES8311_CHANNEL] >> 8;    // 24-bit audio left-aligned in the slot
    return n;
}

#endif // AUDIO_SOURCE == AUDIO_SRC_ES8311
