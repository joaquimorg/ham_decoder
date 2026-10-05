#include "skimmer.h"

// The project builds with -Og; this runs on every sample.
#pragma GCC optimize("O2")

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#include "cw_decoder.h"
#include "psk_decoder.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

namespace {

constexpr float MATCH_HZ = 20.0f;      // a signal this close is the channel's own
constexpr float BUSY_HZ = 40.0f;       // left alone this close to a main decoder
constexpr int RELEASE_REPORTS = 8;     // seconds without its signal before a channel is freed
constexpr int PSK_HOLD_REPORTS = 10;   // CW text ignored this long after PSK was seen
constexpr int IDLE_FLUSH_REPORTS = 3;  // a line is shown after this long without new text
constexpr int LINE_MAX = 64;           // a line is broken (at a space) beyond this

enum Mode { MODE_NONE, MODE_CW, MODE_PSK };

struct Channel {
    bool used = false;
    float hz = 0.0f;
    bool matched = false;
    int missing = 0;
    CwReceiver *cw = nullptr;
    PskReceiver *psk = nullptr;
    Mode mode = MODE_NONE;
    int psk_hold = 0;
    char line[LINE_MAX + 16];
    int len = 0;
    int idle = 0;
};

Channel channels[SKIM_CHANNELS];
bool enabled = true;
EXT_RAM_BSS_ATTR char text[SKIM_TEXT_MAX];    // the internal RAM is short
size_t text_len = 0;

void *alloc_big(size_t n)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);    // the internal RAM is short
#else
    return malloc(n);
#endif
}

void append(const char *s)
{
    while (*s && text_len < sizeof(text))
        text[text_len++] = *s++;
}

// CW from noise or a signal that is not Morse decodes as short random
// letters, mostly E, T and I, and unknown sequences ('_'): a line needs some
// longer words and few unknowns. A keyed data signal (one tone of RTTY) also
// reads as Morse at the speed limit: the speed must be a usual one.
constexpr float CW_WPM_MIN = 8.0f, CW_WPM_MAX = 45.0f;

bool cw_line_plausible(const char *s, float wpm)
{
    if (wpm < CW_WPM_MIN || wpm > CW_WPM_MAX)
        return false;
    int unknown = 0;
    for (const char *p = s; *p; p++)
        unknown += *p == '_';
    int words = 0, short_words = 0, letters = 0;
    for (const char *p = s; *p;) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        int n = 0;
        while (*p && *p != ' ') {
            n++;
            p++;
        }
        words++;
        letters += n;
        short_words += n <= 1;
    }
    return letters >= 4 && short_words * 2 <= words && unknown * 8 <= letters;
}

void emit_line(Channel &c)
{
    while (c.len > 0 && c.line[c.len - 1] == ' ')
        c.len--;
    c.line[c.len] = 0;
    const char *t = c.line;
    while (*t == ' ')
        t++;
    if (*t && (c.mode == MODE_PSK || cw_line_plausible(t, c.cw->wpm()))) {
        char head[32];
        if (c.mode == MODE_PSK)
            snprintf(head, sizeof(head), "%4.0f Hz PSK31  ", c.psk->tone_hz());
        else
            snprintf(head, sizeof(head), "%4.0f Hz CW%3.0f  ", c.hz, c.cw->wpm());
        append(head);
        append(t);
        append("\n");
    }
    c.len = 0;
}

void add_text(Channel &c, const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') {
            emit_line(c);
            continue;
        }
        if (c.len == 0 && *s == ' ')
            continue;
        c.line[c.len++] = *s;
        c.idle = 0;
        if (c.len >= LINE_MAX) {
            // Break at the last space (the rest starts the next line).
            int cut = c.len;
            while (cut > LINE_MAX / 2 && c.line[cut - 1] != ' ')
                cut--;
            char rest[LINE_MAX + 16];
            const int nrest = c.len - cut;
            memcpy(rest, c.line + cut, nrest);
            c.len = cut;
            emit_line(c);
            memcpy(c.line, rest, nrest);
            c.len = nrest;
        }
    }
}

void release(Channel &c)
{
    if (c.len)
        emit_line(c);
    c.used = false;
    c.hz = 0.0f;
    c.cw->set_tone(0.0f);
    c.psk->set_tone(0.0f);
}

}    // namespace

void skimmer_init()
{
    for (Channel &c : channels) {
        if (!c.cw)
            c.cw = new (alloc_big(sizeof(CwReceiver))) CwReceiver();
        if (!c.psk)
            c.psk = new PskReceiver(false);
    }
}

void skimmer_set_enabled(bool on)
{
    if (enabled && !on) {
        for (Channel &c : channels)
            if (c.used)
                release(c);
    }
    enabled = on;
}

bool skimmer_enabled() { return enabled; }

void skimmer_update(const SkimSignal *sig, int n, const float *busy_hz, int nbusy)
{
    if (!enabled)
        return;
    for (Channel &c : channels)
        c.matched = false;

    // Signals: refresh the channel already on each, or start a free one.
    for (int i = 0; i < n; i++) {
        bool busy = false;
        for (int b = 0; b < nbusy; b++)
            busy |= busy_hz[b] > 0.0f && fabsf(sig[i].hz - busy_hz[b]) < BUSY_HZ;
        if (busy)
            continue;
        Channel *own = nullptr, *free_ch = nullptr;
        for (Channel &c : channels) {
            if (c.used && fabsf(c.hz - sig[i].hz) < MATCH_HZ)
                own = &c;
            else if (!c.used && !free_ch)
                free_ch = &c;
        }
        if (own) {
            own->matched = true;
            own->missing = 0;
            // CW ignores small moves; the PSK receiver has its own AFC.
            own->cw->set_tone(sig[i].hz);
            own->hz = sig[i].hz;
        } else if (free_ch) {
            Channel &c = *free_ch;
            c.used = true;
            c.matched = true;
            c.hz = sig[i].hz;
            c.missing = 0;
            c.mode = MODE_NONE;
            c.psk_hold = 0;
            c.len = c.idle = 0;
            c.cw->set_tone(c.hz);
            c.psk->set_tone(c.hz);
        }
    }

    for (Channel &c : channels) {
        if (!c.used)
            continue;
        // Text this second: PSK31 when its squelch is open (and for a while
        // after, as CW decodes garbage from PSK), else CW.
        char t[PSK_TEXT_MAX + 1], tc[CW_TEXT_MAX + 1];
        c.psk->take_text(t, sizeof(t));
        c.cw->take_text(tc, sizeof(tc));
        if (c.psk->active()) {
            if (c.mode != MODE_PSK) {
                c.len = 0;    // the CW text so far was the PSK signal
                c.mode = MODE_PSK;
            }
            c.psk_hold = PSK_HOLD_REPORTS;
            add_text(c, t);
        } else if (c.psk_hold > 0) {
            c.psk_hold--;
        } else if (tc[0]) {
            if (c.mode != MODE_CW && c.len)
                emit_line(c);
            c.mode = MODE_CW;
            add_text(c, tc);
        }
        if (c.len && ++c.idle >= IDLE_FLUSH_REPORTS)
            emit_line(c);
        if (!c.matched && ++c.missing >= RELEASE_REPORTS)
            release(c);
    }
}

void skimmer_process(const float *x, int n)
{
    if (!enabled)
        return;
    for (Channel &c : channels) {
        if (!c.used)
            continue;
        c.cw->process(x, n);
        c.psk->process(x, n);
    }
}

size_t skimmer_take_text(char *buf, size_t size)
{
    if (size == 0)
        return 0;
    const size_t n = text_len < size - 1 ? text_len : size - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    text_len = 0;
    return n;
}

int skimmer_channels()
{
    int k = 0;
    for (const Channel &c : channels)
        k += c.used;
    return k;
}

float skimmer_channel_hz(int i)
{
    if (i < 0 || i >= SKIM_CHANNELS || !channels[i].used)
        return 0.0f;
    return channels[i].mode == MODE_PSK ? channels[i].psk->tone_hz() : channels[i].hz;
}
