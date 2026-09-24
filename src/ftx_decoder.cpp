#include "ftx_decoder.h"

#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "config.h"
#include "web_ui.h"

static const char *TAG = "FTX";

namespace {

struct Slot {
    int handle;
    double start;
};

QueueHandle_t queue = nullptr;
bool ready = false;
volatile int last_count = -1, last_ms = -1;

// Any time after 2024 counts as set (the RTC starts at 1970).
constexpr time_t TIME_VALID = 1704067200;

void on_message(const FtxMessage &m, void *)
{
    web_push_ftx(m);
}

void decode_task(void *)
{
    Slot s;
    for (;;) {
        if (xQueueReceive(queue, &s, portMAX_DELAY) != pdTRUE)
            continue;
        const int64_t t0 = esp_timer_get_time();
        const int n = ftx_core_decode(s.handle, s.start, on_message, nullptr);
        ftx_core_release(s.handle);
        last_ms = (int)((esp_timer_get_time() - t0) / 1000);
        last_count = n;
        ESP_LOGI(TAG, "slot %lld: %d mensagens em %d ms", (long long)s.start, n, last_ms);
    }
}

} // namespace

void ftx_init()
{
    ready = ftx_core_init(DSP_SAMPLE_RATE);
    if (!ready) {
        ESP_LOGE(TAG, "sem memoria para o FT8/FT4");
        return;
    }
    queue = xQueueCreate(2, sizeof(Slot));
    // Core 0 (with Wi-Fi and the web server), low priority: a slot takes up
    // to a few hundred ms to decode and has a whole slot to finish.
    xTaskCreatePinnedToCore(decode_task, "ftx_decode", FTX_TASK_STACK, nullptr, 1, nullptr, 0);
}

bool ftx_time_ok()
{
    return time(nullptr) > TIME_VALID;
}

void ftx_process(const float *x, int n)
{
    if (!ready || ftx_core_protocol() == FTX_OFF || !ftx_time_ok())
        return;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    // The block has just been completed: its first sample is n samples back.
    const double t0 = tv.tv_sec + tv.tv_usec * 1e-6 - (double)n / DSP_SAMPLE_RATE;
    Slot s;
    s.handle = ftx_core_feed(x, n, t0, &s.start);
    if (s.handle >= 0 && xQueueSend(queue, &s, 0) != pdTRUE)
        ftx_core_release(s.handle);    // decoder still busy with older slots
}

int ftx_last_count()
{
    return last_count;
}

int ftx_last_ms()
{
    return last_ms;
}
