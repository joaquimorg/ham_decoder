#include "mfsk_decoder.h"

// The project builds with -Og; this runs on every sample.
#pragma GCC optimize("O3")

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>

#include "fft.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace {

// Baseband: 12 kHz -> 4 kHz complex. With the tone spacing equal to the
// symbol rate and two FFT bins per spacing (as fldigi), a symbol is
// L = 8000 x tones / bandwidth samples at 4 kHz: a power of two for every mode.
constexpr int DECIM = 3;
constexpr float FS_B = (float)DSP_SAMPLE_RATE / DECIM;
static_assert(DSP_SAMPLE_RATE == 12000, "the symbol lengths assume 12 kHz");
constexpr int TAPS = 48;
constexpr float CUT_HZ = 1300.0f;    // tones within +-1000 Hz plus the search
constexpr int L_MAX = 1024;

constexpr int SYNC_INTEG = 4;        // blocks the sync metric is averaged over
constexpr uint64_t SCRAMBLE_OLIVIA = 0xE257E6D0291574ECULL;
constexpr uint64_t SCRAMBLE_CONTESTIA = 0xEDB88320ULL;

void *alloc_big(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);    // the internal RAM is short
#else
    void *p = malloc(n);
#endif
    if (p)
        memset(p, 0, n);
    return p;
}

// Small arrays read over and over (soft bits, the FFT): internal RAM when
// there is room to spare - from PSRAM they missed the cache, which core 1
// shares, and a block decode took 3 to 4 times longer.
void *alloc_fast(size_t n)
{
#ifdef ESP_PLATFORM
    if (n <= 6144 && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > n + 12 * 1024) {
        void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p) {
            memset(p, 0, n);
            return p;
        }
    }
#endif
    return alloc_big(n);
}

void free_big(void *p)
{
#ifdef ESP_PLATFORM
    heap_caps_free(p);
#else
    free(p);
#endif
}

inline uint8_t gray_to_binary(uint8_t g)
{
    g ^= g >> 4;
    g ^= g >> 2;
    g ^= g >> 1;
    return g;
}

// Front end (analysis task): mixer, low-pass, decimation into bb_*, a ring
// the decoder reads (on the board a task on core 0 does the decoding, the
// heavy part; on the PC it runs in line).
constexpr int BB_RING = 8192;    // 2 s at 4 kHz
float fir_h[TAPS];
float fir_r[TAPS], fir_i[TAPS];
int fir_pos = 0, decim_n = 0;
float osc_r = 1.0f, osc_i = 0.0f;
volatile float rot_r = 1.0f, rot_i = 0.0f;
volatile bool fe_on = false;
float *bb_r = nullptr, *bb_i = nullptr;
volatile uint32_t bb_w = 0;      // samples written
uint32_t bb_rd = 0;              // samples the decoder has taken
uint32_t overruns = 0;           // times the decoder fell behind and skipped

// Requested configuration (any task), applied by the decoder.
struct Config { MfskMode mode; int tones, bw; float hz; };
Config req = { MFSK_OFF, 0, 0, 0.0f };
volatile uint32_t req_seq = 0;
uint32_t applied_seq = 0;
#ifdef ESP_PLATFORM
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&mux)
#define UNLOCK() portEXIT_CRITICAL(&mux)
TaskHandle_t worker = nullptr;
#else
#define LOCK()
#define UNLOCK()
#endif

// Decoder configuration and state.
MfskMode mode = MFSK_OFF;
int tones = 0, bandwidth = 0, bps = 0, spb = 0, shift = 0;
float centre = 0.0f;
uint64_t scramble = 0;
int L = 0, hop = 0, margin = 0, noff = 0, nphase = 0;
char name[24] = "";

float *ring_r = nullptr, *ring_i = nullptr;    // last L baseband samples
int ring_pos = 0, hop_n = 0, filled = 0;
float *fr = nullptr, *fi = nullptr, *win = nullptr, *power = nullptr;
ComplexFft ffts[8];    // L = 8 .. 1024, built when first needed (they are kept)
const ComplexFft *fft = nullptr;

// Soft bits as int8 (+-127): a quarter of the memory of floats, which on the
// board live in PSRAM and share the cache with the analysis on core 1.
int8_t *soft = nullptr;         // [slice][offset][spb * bps]
// Per character of a block: where each of its spb soft bits sits relative to
// the oldest symbol (the bit rotates with the symbol) and its scrambling sign.
uint16_t dec_idx[8][64];
int8_t dec_sign[8][64];
int soft_pos[2] = { 0, 0 };     // next symbol slot of each slice
float *sync_sig = nullptr, *sync_noise = nullptr;    // [phase][offset]
uint64_t *pipe_blk = nullptr;   // [phase][offset] characters of the last block
float *pipe_snr = nullptr;      // [phase][offset] its own S/N (the sync metric lags)
int phase = 0, slice = 0;
int track_blocks = 0;          // blocks left in tracking (a signal was found)
uint8_t tone_bits[64];         // bits of each tone (Gray decoded)
int best_phase = 0, best_off = 0;
float best_sig = 0.0f, snr = 0.0f;
bool active = false;

char text[MFSK_TEXT_MAX];
size_t text_len = 0;

void free_state()
{
    for (void *p : { (void *)ring_r, (void *)ring_i, (void *)fr, (void *)fi, (void *)win, (void *)power,
                     (void *)soft, (void *)sync_sig, (void *)sync_noise, (void *)pipe_blk, (void *)pipe_snr })
        if (p)
            free_big(p);
    ring_r = ring_i = fr = fi = win = power = sync_sig = sync_noise = pipe_snr = nullptr;
    soft = nullptr;
    pipe_blk = nullptr;
}

void put_char(uint8_t c)
{
    if (mode == MFSK_CONTESTIA && c > 0) {
        if (c == 59)
            c = ' ';
        else if (c == 60)
            c = '\r';
        else if (c == 61)
            c = 8;
        else
            c += 32;
    }
    c &= 0x7F;
    if (c == '\r')
        c = '\n';
    if ((c >= 0x20 && c < 0x7F) || c == '\n') {
        LOCK();
        if (text_len < sizeof(text))
            text[text_len++] = (char)c;
        UNLOCK();
    }
}

// The tables of decode_block() for the mode set.
void build_decode_tables()
{
    for (int fb = 0; fb < bps; fb++) {
        int rot = fb, code = (fb * shift) & (spb - 1);
        for (int t = 0; t < spb; t++) {
            dec_idx[fb][t] = (uint16_t)(t * bps + rot);
            dec_sign[fb][t] = (scramble >> code) & 1 ? -1 : 1;
            code = (code + 1) & (spb - 1);
            if (++rot >= bps)
                rot -= bps;
        }
    }
}

// Integer fast Hadamard transform (as fht()).
void fht_int(int32_t *d, int n)
{
    for (int step = 1; step < n; step *= 2) {
        for (int p = 0; p < n; p += 2 * step) {
            for (int q = p; q < p + step; q++) {
                const int32_t a = d[q], b = d[q + step];
                d[q] = b + a;
                d[q + step] = b - a;
            }
        }
    }
}

// Decodes the block of decoder `d` (soft bits, spb symbols of bps bits,
// oldest at `pos`): its characters, and the signal (mean Hadamard peak) and
// noise (mean energy of the other outputs), in soft-bit units.
uint64_t decode_block(const int8_t *d, int pos, float *sig, float *noise)
{
    int32_t buf[64];
    uint64_t chars = 0;
    float s = 0.0f, nz = 0.0f;
    const int len = spb * bps, base = pos * bps;
    for (int fb = 0; fb < bps; fb++) {
        int32_t in_sq = 0;
        for (int t = 0; t < spb; t++) {
            int j = base + dec_idx[fb][t];
            if (j >= len)
                j -= len;
            const int32_t v = d[j] * dec_sign[fb][t];
            buf[t] = v;
            in_sq += v * v;
        }
        fht_int(buf, spb);
        int32_t peak = 0;
        int at = 0;
        for (int t = 0; t < spb; t++) {
            if (abs(buf[t]) > abs(peak)) {
                peak = buf[t];
                at = t;
            }
        }
        const uint8_t c = (uint8_t)(at + (peak < 0 ? spb : 0));
        chars |= (uint64_t)c << (8 * fb);
        // The transform keeps the energy (times spb): no need to square its outputs.
        const float p = (float)abs(peak);
        s += p;
        nz += ((float)in_sq * spb - p * p) / (spb - 1);
    }
    *sig = s / bps;
    *noise = nz / bps;
    return chars;
}

// One spectrum (every half symbol): soft decisions for every tuning step,
// a block decoded for each, the sync metrics and, half a block after the
// best block timing, its characters.
void spectrum()
{
    // Window the last L samples (oldest first) and transform.
    for (int k = 0; k < L; k++) {
        const int j = (ring_pos + k) % L;
        fr[k] = ring_r[j] * win[k];
        fi[k] = ring_i[j] * win[k];
    }
    fft->run(fr, fi);
    const int lo = -tones + 1 - margin, hi = tones - 1 + margin;
    for (int b = lo; b <= hi; b++) {
        const int k = (b + L) % L;
        power[b - lo] = fr[k] * fr[k] + fi[k] * fi[k];
    }

    const int len = spb * bps;
    const float w = 1.0f / SYNC_INTEG;
    float slice_best = 0.0f;
    int slice_off = 0;
    // What to decode this time (the block decodes are most of the work):
    // while searching, the blocks of one timing in two (the other timing
    // only keeps its soft bits); once a signal is found, the tuning steps
    // next to its own, at both timings.
    const bool tracking = track_blocks > 0;
    const bool decode = tracking || slice == 0;
    const int o_lo = tracking ? (best_off > 0 ? best_off - 1 : 0) : 0;
    const int o_hi = tracking ? (best_off < noff - 1 ? best_off + 1 : noff - 1) : noff - 1;
    for (int o = 0; o < noff; o++) {
        // Soft bits of this symbol (energies squared, as fldigi), Gray coded.
        int8_t *d = soft + ((size_t)slice * noff + o) * len;
        float acc[8] = {};
        float total = 0.0f;
        for (int t = 0; t < tones; t++) {
            float e = power[o + 2 * t];
            e *= e;
            total += e;
            const uint8_t idx = tone_bits[t];
            for (int b = 0; b < bps; b++)
                acc[b] += (idx >> b) & 1 ? -e : e;
        }
        const float k = total > 0.0f ? 127.0f / total : 0.0f;
        int8_t *sym = d + soft_pos[slice] * bps;
        for (int b = 0; b < bps; b++) {
            const float v = acc[b] * k;
            sym[b] = (int8_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
        }
        if (!decode || o < o_lo || o > o_hi)
            continue;
        float sig, noise;
        const int oldest = (soft_pos[slice] + 1) % spb;
        pipe_blk[phase * noff + o] = decode_block(d, oldest, &sig, &noise);
        pipe_snr[phase * noff + o] = noise > 0.0f ? sig / sqrtf(noise) : 0.0f;
        float &ss = sync_sig[phase * noff + o];
        float &sn = sync_noise[phase * noff + o];
        ss += w * (sig - ss);
        sn += w * (noise - sn);
        if (ss > slice_best) {
            slice_best = ss;
            slice_off = o;
        }
    }
    soft_pos[slice] = (soft_pos[slice] + 1) % spb;

    if (!decode) {
        slice_best = -1.0f;    // nothing measured at this timing
        slice_off = best_off;
    }
    if (phase == best_phase) {
        best_sig = slice_best;
        best_off = slice_off;
    } else if (slice_best > best_sig) {
        best_sig = slice_best;
        best_phase = phase;
        best_off = slice_off;
    }
    int dist = phase - best_phase;
    if (dist < 0)
        dist += nphase;
    if (dist == nphase / 2) {
        const float nz = sync_noise[best_phase * noff + best_off];
        snr = nz > 0.0f ? best_sig / sqrtf(nz) : 0.0f;
        // The averaged metric stays up for a few blocks after the signal
        // ends: the block itself must be clean too.
        active = snr >= MFSK_SNR_MIN && pipe_snr[best_phase * noff + best_off] >= MFSK_SNR_MIN;
        if (snr >= MFSK_SNR_MIN * 0.8f)
            track_blocks = 3;
        else if (track_blocks > 0)
            track_blocks--;
        if (active) {
            uint64_t blk = pipe_blk[best_phase * noff + best_off];
            for (int b = 0; b < bps; b++, blk >>= 8)
                put_char((uint8_t)(blk & 0xFF));
        }
    }
    if (++phase == nphase)
        phase = 0;
    slice ^= 1;
}


// Applies the requested configuration (decoder side): frees and allocates
// the decoder state.
void apply_config(const Config &c)
{
    free_state();
    mode = MFSK_OFF;
    name[0] = 0;
    active = false;
    snr = 0.0f;
    tones = c.tones;
    bandwidth = c.bw;
    centre = c.hz;
    if (c.mode == MFSK_OFF || !mfsk_valid(c.tones, c.bw))
        return;

    bps = 0;
    while ((1 << bps) < c.tones)
        bps++;
    spb = c.mode == MFSK_OLIVIA ? 64 : 32;
    shift = c.mode == MFSK_OLIVIA ? 13 : 5;
    scramble = c.mode == MFSK_OLIVIA ? SCRAMBLE_OLIVIA : SCRAMBLE_CONTESTIA;
    L = 8000 * c.tones / c.bw;
    hop = L / 4;
    margin = MFSK_SEARCH_BINS;
    if (margin > L / 2 - c.tones)
        margin = L / 2 - c.tones;    // the search must stay inside the FFT
    noff = 2 * margin + 1;
    nphase = 2 * spb;

    ring_r = (float *)alloc_big(L * sizeof(float));
    ring_i = (float *)alloc_big(L * sizeof(float));
    fr = (float *)alloc_fast(L * sizeof(float));
    fi = (float *)alloc_fast(L * sizeof(float));
    win = (float *)alloc_fast(L * sizeof(float));
    power = (float *)alloc_big((2 * c.tones + 2 * margin) * sizeof(float));
    soft = (int8_t *)alloc_fast((size_t)2 * noff * spb * bps);
    sync_sig = (float *)alloc_big((size_t)nphase * noff * sizeof(float));
    sync_noise = (float *)alloc_big((size_t)nphase * noff * sizeof(float));
    pipe_blk = (uint64_t *)alloc_big((size_t)nphase * noff * sizeof(uint64_t));
    pipe_snr = (float *)alloc_big((size_t)nphase * noff * sizeof(float));
    if (!ring_r || !ring_i || !fr || !fi || !win || !power || !soft || !sync_sig || !sync_noise || !pipe_blk ||
        !pipe_snr) {
        free_state();
        return;
    }
    int lg = 0;
    while ((8 << lg) < L)
        lg++;
    if (ffts[lg].size() != L && !ffts[lg].init(L)) {
        free_state();
        return;
    }
    fft = &ffts[lg];
    build_decode_tables();
    for (int t = 0; t < c.tones; t++)
        tone_bits[t] = gray_to_binary((uint8_t)t);
    track_blocks = 0;
    for (int k = 0; k < L; k++)
        win[k] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * k / L);
    ring_pos = hop_n = filled = 0;
    soft_pos[0] = soft_pos[1] = 0;
    phase = slice = 0;
    best_phase = best_off = 0;
    best_sig = 0.0f;
    mode = c.mode;
    snprintf(name, sizeof(name), "%s %d/%d", c.mode == MFSK_OLIVIA ? "OLIVIA" : "CONTESTIA", c.tones, c.bw);
}

// Decoder: takes the new baseband samples, a spectrum every hop.
void decode_pending()
{
    if (req_seq != applied_seq) {
        LOCK();
        const Config c = req;
        applied_seq = req_seq;
        UNLOCK();
        apply_config(c);
        bb_rd = bb_w;    // older samples were mixed for another centre
    }
    if (mode == MFSK_OFF) {
        bb_rd = bb_w;
        return;
    }
    uint32_t w = bb_w;
    if (w - bb_rd > (uint32_t)BB_RING) {
        // The decoder fell behind (core 0 busy): start again from now.
        overruns++;
        bb_rd = w;
        filled = 0;
        hop_n = 0;
    }
    for (; bb_rd != w; bb_rd++) {
        const int j = bb_rd % BB_RING;
        ring_r[ring_pos] = bb_r[j];
        ring_i[ring_pos] = bb_i[j];
        if (++ring_pos == L)
            ring_pos = 0;
        if (filled < L)
            filled++;
        if (++hop_n == hop) {
            hop_n = 0;
            if (filled == L)
                spectrum();
        }
    }
}

#ifdef ESP_PLATFORM
void worker_task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        decode_pending();
    }
}
#endif

}    // namespace

bool mfsk_valid(int t, int bw)
{
    if (t < 4 || t > 64 || (t & (t - 1)))
        return false;
    if (bw != 125 && bw != 250 && bw != 500 && bw != 1000 && bw != 2000)
        return false;
    const int l = 8000 * t / bw;
    return l >= 8 && l <= L_MAX && !(l & (l - 1));
}

void mfsk_init()
{
    const float fc = CUT_HZ / DSP_SAMPLE_RATE;
    float sum = 0.0f;
    for (int k = 0; k < TAPS; k++) {
        const float m = k - (TAPS - 1) / 2.0f;
        const float sinc = m == 0.0f ? 2.0f * fc : sinf(2.0f * (float)M_PI * fc * m) / ((float)M_PI * m);
        const float bw = 0.42f - 0.5f * cosf(2.0f * (float)M_PI * k / (TAPS - 1)) +
                         0.08f * cosf(4.0f * (float)M_PI * k / (TAPS - 1));    // Blackman
        fir_h[k] = sinc * bw;
        sum += fir_h[k];
    }
    for (int k = 0; k < TAPS; k++)
        fir_h[k] /= sum;
    if (!bb_r) {
        bb_r = (float *)alloc_big(BB_RING * sizeof(float));
        bb_i = (float *)alloc_big(BB_RING * sizeof(float));
    }
#ifdef ESP_PLATFORM
    if (!worker)
        xTaskCreatePinnedToCore(worker_task, "mfsk", MFSK_TASK_STACK, nullptr, 1, &worker, 0);
#endif
}

void mfsk_configure(MfskMode m, int t, int bw, float centre_hz)
{
    LOCK();
    const bool same = m == req.mode && t == req.tones && bw == req.bw && centre_hz == req.hz;
    if (!same) {
        req = { m, t, bw, centre_hz };
        req_seq = req_seq + 1;
    }
    UNLOCK();
    if (same)
        return;
    const float wo = 2.0f * (float)M_PI * centre_hz / DSP_SAMPLE_RATE;
    rot_r = cosf(wo);
    rot_i = -sinf(wo);
    fe_on = m != MFSK_OFF && mfsk_valid(t, bw);
#ifndef ESP_PLATFORM
    decode_pending();    // on the PC everything runs in line
#endif
}

uint32_t mfsk_overruns() { return overruns; }

MfskMode mfsk_mode() { return mode; }
const char *mfsk_name() { return name; }
float mfsk_snr() { return snr; }
float mfsk_offset_hz() { return (best_off - margin) * FS_B / (L ? L : 1); }
bool mfsk_active() { return mode != MFSK_OFF && active; }

void mfsk_process(const float *x, int n)
{
    if (!fe_on || !bb_r)
        return;
    const float rr = rot_r, ri = rot_i;
    uint32_t w = bb_w;
    for (int i = 0; i < n; i++) {
        fir_r[fir_pos] = x[i] * osc_r;
        fir_i[fir_pos] = x[i] * osc_i;
        if (++fir_pos == TAPS)
            fir_pos = 0;
        const float r = osc_r * rr - osc_i * ri;
        osc_i = osc_r * ri + osc_i * rr;
        osc_r = r;
        if (++decim_n < DECIM)
            continue;
        decim_n = 0;
        float yr = 0.0f, yi = 0.0f;
        int j = fir_pos;
        for (int k = 0; k < TAPS; k++) {
            if (--j < 0)
                j = TAPS - 1;
            yr += fir_h[k] * fir_r[j];
            yi += fir_h[k] * fir_i[j];
        }
        bb_r[w % BB_RING] = yr;
        bb_i[w % BB_RING] = yi;
        w++;
    }
    bb_w = w;
    const float g = 1.0f / sqrtf(osc_r * osc_r + osc_i * osc_i);    // keep |osc| = 1
    osc_r *= g;
    osc_i *= g;
#ifdef ESP_PLATFORM
    if (worker)
        xTaskNotifyGive(worker);
#else
    decode_pending();
#endif
}

size_t mfsk_take_text(char *buf, size_t size)
{
    if (size == 0)
        return 0;
    LOCK();
    const size_t n = text_len < size - 1 ? text_len : size - 1;
    memcpy(buf, text, n);
    text_len = 0;
    UNLOCK();
    buf[n] = 0;
    return n;
}
