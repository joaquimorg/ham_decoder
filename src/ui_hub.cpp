#include "ui_hub.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "UI";

// ---------------------------------------------------------------------------
// Shared state (producer tasks write, the interfaces read), guarded by `lock`.

#define ROWS 64                 // ~5 s of spectrum rows at ~12 rows/s

static SemaphoreHandle_t lock;

EXT_RAM_BSS_ATTR static uint8_t rows[ROWS][UI_BINS];
static uint32_t row_seq = 0;    // number of rows ever pushed

struct TextRing {
    char buf[UI_TEXT_RING];
    uint32_t seq = 0;           // number of characters ever pushed
};
static TextRing texts[UI_TEXT_COUNT];    // UiTextChannel: CW, RTTY, PSK, APRS, POCSAG, DTMF/CTCSS

static UiStatus status;
static volatile float analysis_load = 0.0f;
static const char *boot_reason = "";
static bool boot_unexpected = false;
static unsigned boot_resets = 0;

void ui_set_boot_info(const char *reason, bool unexpected, unsigned resets)
{
    boot_reason = reason;
    boot_unexpected = unexpected;
    boot_resets = resets;
}

const char *ui_boot_reason() { return boot_reason; }
bool ui_boot_unexpected() { return boot_unexpected; }
unsigned ui_boot_resets() { return boot_resets; }

void ui_set_load(float fraction)
{
    analysis_load = fraction;
}

float ui_load()
{
    return analysis_load;
}

void ui_push_spectrum(const uint8_t *row)
{
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(rows[row_seq % ROWS], row, UI_BINS);
    row_seq++;
    xSemaphoreGive(lock);
}

void ui_push_status(const UiStatus &st)
{
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    status = st;
    xSemaphoreGive(lock);
}

void ui_push_text(UiTextChannel ch, const char *text)
{
    if (!lock || !text[0])
        return;
    TextRing &t = texts[ch];
    xSemaphoreTake(lock, portMAX_DELAY);
    for (const char *p = text; *p; p++)
        t.buf[t.seq++ % UI_TEXT_RING] = *p;
    xSemaphoreGive(lock);
}

// Image lines: a ring in PSRAM when there is some (a whole FAX chart fits), so
// a viewer opened mid-image still gets it all; a small one in internal RAM
// otherwise. `img_id` changes with every new image.
#define IMG_RING_PSRAM (1536 * 1024)
#define IMG_RING_RAM   (24 * 1024)
#define IMG_TITLE      32

static uint8_t *img_ring = nullptr;
static size_t img_ring_bytes = 0;
static uint32_t img_id = 0, img_lines = 0, img_cap = 0;
static uint16_t img_width = 0;
static uint8_t img_channels = 1;
static bool img_active = false;
static float img_aspect = 1.0f;
static char img_title[IMG_TITLE] = "";

static void image_alloc()
{
    img_ring = (uint8_t *)heap_caps_malloc(IMG_RING_PSRAM, MALLOC_CAP_SPIRAM);
    img_ring_bytes = IMG_RING_PSRAM;
    if (!img_ring) {
        img_ring = (uint8_t *)malloc(IMG_RING_RAM);
        img_ring_bytes = img_ring ? IMG_RING_RAM : 0;
    }
    ESP_LOGI(TAG, "imagem: %u KB", (unsigned)(img_ring_bytes / 1024));
}

uint32_t ui_image_begin(const char *title, int width, int channels, float aspect)
{
    if (!lock || !img_ring || width * channels > UI_IMG_MAX_LINE)
        return 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    const uint32_t id = ++img_id;
    img_lines = 0;
    img_width = width;
    img_channels = channels;
    img_cap = img_ring_bytes / (width * channels);
    img_aspect = aspect;
    img_active = true;
    strlcpy(img_title, title, sizeof(img_title));
    xSemaphoreGive(lock);
    return id;
}

void ui_image_line(uint32_t id, const uint8_t *px)
{
    if (!lock || !img_ring || !id)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (id == img_id && img_active) {
        const size_t bytes = img_width * img_channels;
        memcpy(img_ring + (img_lines % img_cap) * bytes, px, bytes);
        img_lines++;
    }
    xSemaphoreGive(lock);
}

uint32_t ui_image_rotate(uint32_t id, int px)
{
    if (!lock || !img_ring)
        return id;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (id == img_id && img_width > 0) {
        px = ((px % img_width) + img_width) % img_width;
        const size_t bytes = (size_t)img_width * img_channels, cut = (size_t)px * img_channels;
        static uint8_t tmp[UI_IMG_MAX_LINE];
        const uint32_t held = img_lines < img_cap ? img_lines : img_cap;
        for (uint32_t k = img_lines - held; k < img_lines && cut; k++) {
            uint8_t *line = img_ring + (k % img_cap) * bytes;
            memcpy(tmp, line, cut);
            memmove(line, line + cut, bytes - cut);
            memcpy(line + bytes - cut, tmp, cut);
        }
        id = ++img_id;
    }
    xSemaphoreGive(lock);
    return id;
}

void ui_image_end(uint32_t id)
{
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (id == img_id)
        img_active = false;
    xSemaphoreGive(lock);
}

UiImageInfo ui_get_image_info()
{
    UiImageInfo info = {};
    if (!lock)
        return info;
    xSemaphoreTake(lock, portMAX_DELAY);
    info.id = img_id;
    info.lines = img_lines;
    info.first = img_lines > img_cap ? img_lines - img_cap : 0;
    info.width = img_width;
    info.channels = img_channels;
    info.active = img_active;
    info.aspect = img_aspect;
    strlcpy(info.title, img_title, sizeof(info.title));
    xSemaphoreGive(lock);
    return info;
}

bool ui_get_image_lines(uint32_t id, uint32_t first, uint32_t n, uint8_t *out)
{
    if (!lock || !img_ring)
        return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    const size_t bytes = (size_t)img_width * img_channels;
    const bool held = id == img_id && first + n <= img_lines && img_lines - first <= img_cap;
    if (held)
        for (uint32_t j = 0; j < n; j++)
            memcpy(out + j * bytes, img_ring + ((first + j) % img_cap) * bytes, bytes);
    xSemaphoreGive(lock);
    return held;
}

// Gallery: the last UI_GALLERY finished images (SSTV), each in its own
// PSRAM buffer. `gal_seq` numbers them; a slot is reused for the newest.
struct GalleryImg {
    uint32_t n = 0;             // gal_seq when stored, 0 = empty
    uint8_t *px = nullptr;
    uint16_t w = 0, h = 0;
    int64_t utc = 0;
    char title[IMG_TITLE] = "";
};
static GalleryImg gallery[UI_GALLERY];
static uint32_t gal_seq = 0;

void ui_image_archive(uint32_t id)
{
    if (!lock || !img_ring)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (id == img_id && img_channels == 3 && img_lines > 0 && img_lines <= img_cap) {
        const size_t bytes = (size_t)img_width * 3;
        uint8_t *px = (uint8_t *)heap_caps_malloc(bytes * img_lines, MALLOC_CAP_SPIRAM);
        if (px) {
            GalleryImg &g = gallery[gal_seq % UI_GALLERY];
            free(g.px);
            memcpy(px, img_ring, bytes * img_lines);    // lines 0..n-1 sit at the ring start
            g.px = px;
            g.w = img_width;
            g.h = img_lines;
            const time_t now = time(nullptr);
            g.utc = now > 1704067200 ? now : 0;
            strlcpy(g.title, img_title, sizeof(g.title));
            g.n = ++gal_seq;
        }
    }
    xSemaphoreGive(lock);
}

uint32_t ui_gallery_seq()
{
    return gal_seq;
}

int ui_gallery_list(UiGalleryEntry *out)
{
    int n = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (const GalleryImg &g : gallery) {
        if (!g.n)
            continue;
        UiGalleryEntry &e = out[n++];
        e.n = g.n;
        e.w = g.w;
        e.h = g.h;
        e.utc = g.utc;
        strlcpy(e.title, g.title, sizeof(e.title));
    }
    xSemaphoreGive(lock);
    return n;
}

int ui_gallery_read(uint32_t n, size_t off, uint8_t *out, size_t max)
{
    const GalleryImg &g = gallery[(n + UI_GALLERY - 1) % UI_GALLERY];
    xSemaphoreTake(lock, portMAX_DELAY);
    int got = -1;
    if (n && g.n == n) {
        const size_t total = (size_t)g.w * g.h * 3;
        const size_t cnt = off < total ? (total - off < max ? total - off : max) : 0;
        memcpy(out, g.px + off, cnt);
        got = (int)cnt;
    }
    xSemaphoreGive(lock);
    return got;
}

// FT8/FT4 messages: ring of the last UI_FTX_RING, numbered by `ftx_seq`.
EXT_RAM_BSS_ATTR static FtxMessage ftx_ring[UI_FTX_RING];
static uint32_t ftx_seq = 0;

void ui_push_ftx(const FtxMessage &m)
{
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    ftx_ring[ftx_seq++ % UI_FTX_RING] = m;
    xSemaphoreGive(lock);
}

// ---------------------------------------------------------------------------
// Consumers

int ui_get_rows(uint32_t from, uint8_t (*out)[UI_BINS], int max, uint32_t *next)
{
    if (max > ROWS)
        max = ROWS;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (from > row_seq || row_seq - from > (uint32_t)max)
        from = row_seq > (uint32_t)max ? row_seq - max : 0;    // new reader or fell behind
    const int n = row_seq - from;
    for (int i = 0; i < n; i++)
        memcpy(out[i], rows[(from + i) % ROWS], UI_BINS);
    *next = row_seq;
    xSemaphoreGive(lock);
    return n;
}

UiStatus ui_get_status()
{
    xSemaphoreTake(lock, portMAX_DELAY);
    const UiStatus st = status;
    xSemaphoreGive(lock);
    return st;
}

int ui_get_text(UiTextChannel ch, uint32_t from, char *out, uint32_t *next)
{
    const TextRing &t = texts[ch];
    xSemaphoreTake(lock, portMAX_DELAY);
    if (from > t.seq || t.seq - from > UI_TEXT_RING)
        from = t.seq > 512 ? t.seq - 512 : 0;
    const int n = t.seq - from;
    for (int i = 0; i < n; i++)
        out[i] = t.buf[(from + i) % UI_TEXT_RING];
    out[n] = 0;
    *next = t.seq;
    xSemaphoreGive(lock);
    return n;
}

int ui_get_ftx(uint32_t from, FtxMessage *out, uint32_t *next)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    const uint32_t seq = ftx_seq;
    if (from > seq || seq - from > UI_FTX_RING)
        from = seq > UI_FTX_RING ? seq - UI_FTX_RING : 0;
    const int n = seq - from;
    for (int i = 0; i < n; i++)
        out[i] = ftx_ring[(from + i) % UI_FTX_RING];
    *next = seq;
    xSemaphoreGive(lock);
    return n;
}

void ui_hub_init()
{
    image_alloc();
    lock = xSemaphoreCreateMutex();
}
