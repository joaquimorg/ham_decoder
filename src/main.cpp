#include <stdio.h>
#include <inttypes.h>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "config.h"
#include "analyzer.h"
#include "mfsk_decoder.h"
#include "audio_monitor.h"
#include "audio_source.h"
#include "board_i2c.h"
#include "settings.h"
#include "ui_hub.h"
#include "web_ui.h"
#include "lcd_ui.h"

static const char *TAG = "RX_ANALYZER";

// Capture -> analysis hand-off: DSP_NUM_BUFFERS blocks cycle through two queues.
static float dsp_buffers[DSP_NUM_BUFFERS][FFT_SIZE];
static QueueHandle_t free_queue = nullptr;
static QueueHandle_t full_queue = nullptr;

struct BlockMsg {
    int index;
    int32_t raw_peak;
    uint32_t overruns;
};

static void audio_task(void *arg)
{
    static int32_t samples[AUDIO_BLOCK_SAMPLES];

    const float scale = 1.0f / (DECIM_FACTOR * AUDIO_FULL_SCALE);
    float dc = 0.0f;

    int32_t decim_acc = 0;
    int decim_n = 0;

    int cur = 0;
    int pos = 0;
    int32_t peak = 0;
    uint32_t overruns = 0;

    while (true) {
        const size_t n = audio_source_read(samples, AUDIO_BLOCK_SAMPLES);
        audio_monitor_write(samples, n);

        for (size_t i = 0; i < n; i++) {
            int32_t x = samples[i];

            int32_t ax = x < 0 ? -x : x;
            if (ax > peak)
                peak = ax;

            // Boxcar decimation AUDIO_SAMPLE_RATE -> DSP_SAMPLE_RATE.
            decim_acc += x;
            if (++decim_n < DECIM_FACTOR)
                continue;

            float y = decim_acc * scale;
            decim_acc = 0;
            decim_n = 0;

            // DC removal (one-pole high-pass, ~2 Hz corner).
            dc += (y - dc) * 0.001f;
            dsp_buffers[cur][pos++] = y - dc;

            if (pos < FFT_SIZE)
                continue;
            pos = 0;

            int next;
            if (xQueueReceive(free_queue, &next, 0) != pdTRUE) {
                // Analysis is behind: drop this block and refill it.
                overruns++;
                continue;
            }

            BlockMsg msg = { cur, peak, overruns };
            xQueueSend(full_queue, &msg, portMAX_DELAY);
            cur = next;
            peak = 0;
        }
    }
}

static void analysis_task(void *arg)
{
    analyzer_init();

    // Share of core 1 spent analysing: busy time per block over the block
    // period (FFT_SIZE samples at DSP_SAMPLE_RATE), averaged over ~1 s.
    const float block_us = 1e6f * FFT_SIZE / DSP_SAMPLE_RATE;
    float load = 0.0f;

    while (true) {
        BlockMsg msg;
        xQueueReceive(full_queue, &msg, portMAX_DELAY);
        const int64_t t0 = esp_timer_get_time();
        analyzer_process_block(dsp_buffers[msg.index], msg.raw_peak, msg.overruns);
        load += ((esp_timer_get_time() - t0) / block_us - load) * 0.1f;
        ui_set_load(load);
#if LOAD_LOG_S
        static int64_t load_peak_us = 0, load_last_log = 0;
        load_peak_us = std::max(load_peak_us, esp_timer_get_time() - t0);
        if (t0 - load_last_log >= LOAD_LOG_S * 1000000LL) {
            ESP_LOGW("LOAD", "analise %.0f %% (pico de um bloco %.0f %%), Olivia atrasou-se %u vezes", load * 100.0f,
                     load_peak_us / block_us * 100.0f, (unsigned)mfsk_overruns());
            ESP_LOGW("LOAD", "RAM interna livre %u KB (maior bloco %u KB)",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));

            load_last_log = t0;
            load_peak_us = 0;
        }
#endif
        xQueueSend(free_queue, &msg.index, portMAX_DELAY);
        audio_source_log();
    }
}

static const char *reset_reason_text(esp_reset_reason_t r, bool *unexpected)
{
    *unexpected = true;
    switch (r) {
    case ESP_RST_POWERON:   *unexpected = false; return "ligação da alimentação";
    case ESP_RST_EXT:       *unexpected = false; return "botão de reset";
    case ESP_RST_SW:        *unexpected = false; return "reinício pedido";
    case ESP_RST_USB:       *unexpected = false; return "USB (programação)";
    case ESP_RST_JTAG:      *unexpected = false; return "JTAG";
    case ESP_RST_DEEPSLEEP: *unexpected = false; return "deep sleep";
    case ESP_RST_PANIC:     return "erro de software (panic)";
    case ESP_RST_INT_WDT:   return "watchdog de interrupções";
    case ESP_RST_TASK_WDT:  return "watchdog de tarefas";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "quebra de tensão (brownout)";
    case ESP_RST_PWR_GLITCH: return "falha na alimentação";
    case ESP_RST_CPU_LOCKUP: return "CPU bloqueado";
    default:                return "motivo desconhecido";
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, " RX Analyzer - V0.2");
#if AUDIO_SOURCE == AUDIO_SRC_PCM1808
    ESP_LOGI(TAG, " " BOARD_NAME " + PCM1808");
#elif AUDIO_SOURCE == AUDIO_SRC_ES8311
    ESP_LOGI(TAG, " " BOARD_NAME " + ES8311");
#else
    ESP_LOGI(TAG, " " BOARD_NAME " + ADC interno");
#endif
    ESP_LOGI(TAG, "=================================");

    settings_init();
    ui_hub_init();
    settings_apply();

    // Why did we (re)start? Logged as a warning so it stays visible, counted
    // in NVS when unexpected, and shown on the web page.
    bool unexpected;
    const char *reason = reset_reason_text(esp_reset_reason(), &unexpected);
    const unsigned resets = settings_count_reset(unexpected);
    ESP_LOGW(TAG, "arranque: %s (reinicios inesperados ate agora: %u)", reason, resets);
    ui_set_boot_info(reason, unexpected, resets);

    free_queue = xQueueCreate(DSP_NUM_BUFFERS, sizeof(int));
    full_queue = xQueueCreate(DSP_NUM_BUFFERS, sizeof(BlockMsg));
    for (int i = 1; i < DSP_NUM_BUFFERS; i++)
        xQueueSend(free_queue, &i, 0);

#if defined(BOARD_FNK0104S)
    board_i2c_bus();    // touch and ES8311 share it
#endif
    audio_source_init();
    audio_monitor_init();

    // Audio and analysis on core 1; Wi-Fi and the web server stay on core 0.
    xTaskCreatePinnedToCore(analysis_task, "analysis_task", 6144, nullptr, 4, nullptr, 1);
    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, nullptr, 6, nullptr, 1);

    lcd_ui_start();
    if (WEB_UI && g_settings.web_enabled)
        web_start();
    else
        ESP_LOGW(TAG, "Wi-Fi e pagina web desligados");

    // After start-up only warnings and errors reach the console (the web
    // address is logged as a warning).
    esp_log_level_set("*", ESP_LOG_WARN);
    // A browser closing a connection mid-response (tab closed, phone asleep,
    // request timed out) is routine, not worth a warning.
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_ws", ESP_LOG_ERROR);
}
