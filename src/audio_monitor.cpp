#include "audio_monitor.h"

#if AUDIO_MONITOR

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "es8311.h"

static const char *TAG = "MONITOR";

#define MON_BLOCK       128                 // samples per I2S write
#define MON_BUFFER      (16 * MON_BLOCK)    // ~43 ms of queued audio at most

static StreamBufferHandle_t queue = nullptr;
static volatile bool running = false;       // volume > 0 and the output task is up
static int volume = MONITOR_DEFAULT_VOL;

// The queue decouples the capture task from the I2S output, which with the
// internal ADC runs on another clock: when it fills up, samples are dropped.
static void monitor_task(void *)
{
    static int32_t mono[MON_BLOCK];
    static int32_t stereo[MON_BLOCK * 2];

    while (true) {
        const size_t bytes = xStreamBufferReceive(queue, mono, sizeof(mono), pdMS_TO_TICKS(50));
        const size_t n = bytes / sizeof(int32_t);
        if (n == 0)
            continue;    // nothing queued (monitor off): the DMA sends zeros
        for (size_t i = 0; i < n; i++)
            stereo[2 * i] = stereo[2 * i + 1] = mono[i] << 8;    // 24 bits left-aligned
        size_t written;
        i2s_channel_write(es8311_tx(), stereo, n * 2 * sizeof(int32_t), &written, pdMS_TO_TICKS(100));
    }
}

void audio_monitor_init()
{
    es8311_start();
    es8311_set_volume(volume);
    // In PSRAM: the internal RAM is needed by the Wi-Fi buffers.
    static StaticStreamBuffer_t queue_struct;
    uint8_t *storage = (uint8_t *)heap_caps_malloc(MON_BUFFER * sizeof(int32_t) + 1, MALLOC_CAP_SPIRAM);
    if (storage)
        queue = xStreamBufferCreateStatic(MON_BUFFER * sizeof(int32_t), MON_BLOCK * sizeof(int32_t),
                                          storage, &queue_struct);
    if (!queue) {
        ESP_LOGE(TAG, "sem memoria para a fila de audio");
        return;
    }
    xTaskCreatePinnedToCore(monitor_task, "monitor_task", 3072, nullptr, 5, nullptr, 1);
    running = volume > 0;
}

void audio_monitor_write(const int32_t *samples, size_t n)
{
    if (!running)
        return;
    xStreamBufferSend(queue, samples, n * sizeof(int32_t), 0);
}

void audio_monitor_set_volume(int percent)
{
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (percent == volume)
        return;
    volume = percent;
    es8311_set_volume(volume);
    running = queue && volume > 0;
}

#endif
