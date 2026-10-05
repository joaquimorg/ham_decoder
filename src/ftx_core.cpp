#include "ftx_core.h"

#include <atomic>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <common/monitor.h>
#include <ft8/decode.h>
#include <ft8/encode.h>
#include <ft8/message.h>

#include "js8_decoder.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

// Validated on a PC with tools/ftx_test (synthetic FT8/FT4 with noise, several
// signals per slot, arbitrary start times). Keep both in sync.

namespace {

// Decoder settings as in ft8_lib's demo/decode_ft8.c.
constexpr int MIN_SCORE = 10;
constexpr int MAX_CANDIDATES = 140;
constexpr int LDPC_ITERATIONS = 25;
constexpr int MAX_DECODED = 50;
constexpr int FREQ_OSR = 2, TIME_OSR = 2;
constexpr float F_MIN = 200.0f, F_MAX = 3000.0f;
constexpr float SNR_CAL = 1.7f;    // dB, see estimate_snr()

// The waterfall starts time_shift into the slot (FT8: 0.8 s, as in
// decode_ft8.c): transmissions start nominal (0.5 s) in, and the candidate
// search looks back. JS8: the start delay of the speed plus two symbols.
// A slot is only started within this long of its beginning (no partial slots).
constexpr double START_WINDOW = 0.25;

enum BufState { FREE = 0, FILLING = 1, READY = 2 };

struct Buffer {
    monitor_t mon;
    std::atomic<int> state{FREE};
};

struct Proto {
    bool ok = false;
    int protocol = FTX_OFF;       // the one the waterfalls are set up for
    double period = 0.0;
    double time_shift = 0.8, nominal = 0.5;
    Buffer buf[2];
    float *frame = nullptr;       // block_size samples for monitor_process()
    int frame_n = 0;
    int filling = -1;             // buffer being filled
    int64_t last_slot = -1;       // slot index last started (or skipped)
    double slot_start = 0.0;
};

Proto protos[3];                  // FT8, FT4, JS8 (one speed at a time)
std::atomic<int> req_protocol{FTX_OFF};
int protocol = FTX_OFF;
int fs = 12000;
std::atomic<int> skipped{0};

bool is_js8(int p)
{
    return p >= FTX_JS8 && p <= FTX_JS8_SLOW;
}

int proto_index(int p)
{
    return p == FTX_FT4 ? 1 : is_js8(p) ? 2 : 0;
}

Proto &proto_of(int p)
{
    return protos[proto_index(p)];
}

const Js8SubmodeInfo &js8_info(int p)
{
    return js8_submode((Js8Submode)(p - FTX_JS8));
}

void proto_free(Proto &P)
{
    if (!P.protocol)
        return;
    for (Buffer &b : P.buf)
        monitor_free(&b.mon);
    free(P.frame);
    P.frame = nullptr;
    P.ok = false;
    P.protocol = FTX_OFF;
}

bool proto_init(int p)
{
    Proto &P = proto_of(p);
    proto_free(P);
    monitor_config_t cfg = {};
    cfg.f_min = F_MIN;
    cfg.f_max = F_MAX;
    cfg.sample_rate = fs;
    cfg.time_osr = TIME_OSR;
    cfg.freq_osr = FREQ_OSR;
    cfg.protocol = p == FTX_FT4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
    P.period = p == FTX_FT4 ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    P.time_shift = 0.8;
    P.nominal = 0.5;
    if (is_js8(p)) {
        const Js8SubmodeInfo &si = js8_info(p);
        cfg.symbol_period = si.symbol_period;
        cfg.slot_time = si.slot;
        P.period = si.slot;
        P.nominal = si.start_delay;
        P.time_shift = si.start_delay + 2.0 * si.symbol_period;
    }
    P.protocol = p;
    for (Buffer &b : P.buf) {
        monitor_init(&b.mon, &cfg);
        if (!b.mon.wf.mag || !b.mon.window || !b.mon.last_frame || !b.mon.timedata ||
            !b.mon.freqdata || !b.mon.fft_work)
            return false;
    }
    P.frame = (float *)malloc(P.buf[0].mon.block_size * sizeof(float));
    P.filling = -1;
    P.last_slot = -1;
    P.ok = P.frame != nullptr;
    return P.ok;
}

// Stops filling (protocol change): the partial slot is dropped.
void abandon(Proto &P)
{
    if (P.filling >= 0)
        P.buf[P.filling].state = FREE;
    P.filling = -1;
}

// ---------------------------------------------------------------------------
// Callsign hash table for messages that carry hashed callsigns (from ft8_lib's
// demo/decode_ft8.c, MIT). Used by the decoding task only.

constexpr int HASH_SIZE = 256;
struct HashEntry {
    char callsign[12];
    uint32_t hash;    // 8 MSBs: age; 22 LSBs: hash
};
EXT_RAM_BSS_ATTR HashEntry hash_table[HASH_SIZE];

void hash_cleanup(uint8_t max_age)
{
    for (HashEntry &e : hash_table) {
        if (!e.callsign[0])
            continue;
        const uint8_t age = (uint8_t)(e.hash >> 24);
        if (age > max_age) {
            e.callsign[0] = 0;
            e.hash = 0;
        } else {
            e.hash = (((uint32_t)age + 1u) << 24) | (e.hash & 0x3FFFFFu);
        }
    }
}

void hash_add(const char *callsign, uint32_t hash)
{
    int i = (((hash >> 12) & 0x3FFu) * 23) % HASH_SIZE;
    for (int n = 0; n < HASH_SIZE && hash_table[i].callsign[0]; n++) {
        if ((hash_table[i].hash & 0x3FFFFFu) == hash && !strcmp(hash_table[i].callsign, callsign)) {
            hash_table[i].hash &= 0x3FFFFFu;    // seen again: age 0
            return;
        }
        i = (i + 1) % HASH_SIZE;
    }
    if (hash_table[i].callsign[0])
        return;    // table full
    strncpy(hash_table[i].callsign, callsign, 11);
    hash_table[i].callsign[11] = 0;
    hash_table[i].hash = hash;
}

bool hash_lookup(ftx_callsign_hash_type_t type, uint32_t hash, char *callsign)
{
    const uint8_t shift = type == FTX_CALLSIGN_HASH_10_BITS ? 12 : type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0;
    int i = (((hash >> (12 - shift)) & 0x3FFu) * 23) % HASH_SIZE;
    for (int n = 0; n < HASH_SIZE && hash_table[i].callsign[0]; n++) {
        if (((hash_table[i].hash & 0x3FFFFFu) >> shift) == hash) {
            strcpy(callsign, hash_table[i].callsign);
            return true;
        }
        i = (i + 1) % HASH_SIZE;
    }
    callsign[0] = 0;
    return false;
}

ftx_callsign_hash_interface_t hash_if = { hash_lookup, hash_add };

// Noise power per waterfall entry: median of the whole slot (signals cover
// few of its bins), times 1/ln 2 for the mean of exponentially distributed
// noise power.
float noise_power(const ftx_waterfall_t &wf)
{
    EXT_RAM_BSS_ATTR static uint32_t hist[256];
    memset(hist, 0, sizeof(hist));
    const int n = wf.num_blocks * wf.block_stride;
    for (int i = 0; i < n; i++)
        hist[WF_ELEM_MAG_INT(wf.mag[i]) & 255]++;
    uint32_t sum = 0;
    int med = 0;
    while (med < 255 && (sum += hist[med]) < (uint32_t)n / 2)
        med++;
    return powf(10.0f, WF_ELEM_MAG(med) / 10.0f) / 0.6931f;
}

// SNR in 2500 Hz: mean power in the transmitted tone above the noise, scaled
// from the FFT bin's noise bandwidth (Hann window: 1.5 bins) to 2500 Hz. The
// analysis window spans two symbols, so a tone keeps only part of its power
// in its bin; SNR_CAL corrects that (measured with tools/ftx_test).
float estimate_snr(const monitor_t &mon, const ftx_candidate_t &c, const uint8_t *tones, float pn)
{
    const ftx_waterfall_t &wf = mon.wf;
    const bool ft4 = wf.protocol == FTX_PROTOCOL_FT4;
    const int n_sym = ft4 ? FT4_NN : FT8_NN, n_tones = ft4 ? 4 : 8;
    const WF_ELEM_T *base = wf.mag + c.time_sub * wf.freq_osr * wf.num_bins +
                            c.freq_sub * wf.num_bins + c.freq_offset;
    float sig = 0.0f;
    int n_sig = 0;
    for (int s = ft4 ? 1 : 0; s < (ft4 ? n_sym - 1 : n_sym); s++) {
        const int block = c.time_offset + s;
        if (block < 0 || block >= wf.num_blocks)
            continue;
        const WF_ELEM_T *p = base + block * wf.block_stride;
        const int t = tones[s];
        if (t < n_tones && c.freq_offset + t < wf.num_bins) {
            sig += powf(10.0f, WF_ELEM_MAG(p[t]) / 10.0f);
            n_sig++;
        }
    }
    if (!n_sig)
        return -30.0f;
    const float ps = sig / n_sig;
    const float enbw = 1.5f / (mon.symbol_period * wf.freq_osr);
    const float sp = ps - pn > 1e-3f * pn ? ps - pn : 1e-3f * pn;
    return 10.0f * log10f(sp / pn * enbw / 2500.0f) + SNR_CAL;
}

// Time, frequency and SNR of a decoded candidate.
void fill_message(const Proto &P, const monitor_t &mon, const ftx_candidate_t &c, double slot_start,
                  const uint8_t *tones, float pn, FtxMessage &out)
{
    const ftx_waterfall_t *wf = &mon.wf;
    const float time_s = (c.time_offset + (float)c.time_sub / wf->time_osr) * mon.symbol_period;
    out.slot_start = slot_start;
    out.freq_hz = (mon.min_bin + c.freq_offset + (float)c.freq_sub / wf->freq_osr) / mon.symbol_period;
    // The candidate time is one symbol late (analysis window alignment).
    out.dt = time_s + (float)(P.time_shift - P.nominal) - mon.symbol_period;
    out.snr_db = estimate_snr(mon, c, tones, pn);
}

// JS8: its own sync arrays, code and frames (js8_decoder.h).
int js8_decode(const Proto &P, const monitor_t &mon, double slot_start, FtxMessageCb cb, void *ctx)
{
    struct Frame { uint8_t payload[9]; int i3; };
    EXT_RAM_BSS_ATTR static ftx_candidate_t cands[MAX_CANDIDATES];    // decoding task only
    EXT_RAM_BSS_ATTR static Frame decoded[MAX_DECODED];
    const bool original = js8_info(P.protocol).original_costas;
    const int n_cand = js8_find_candidates(&mon.wf, original, MAX_CANDIDATES, cands, MIN_SCORE);
    const float pn = n_cand ? noise_power(mon.wf) : 1.0f;
    int n_dec = 0;
    for (int i = 0; i < n_cand && n_dec < MAX_DECODED; i++) {
        Frame f;
        if (!js8_decode_candidate(&mon.wf, &cands[i], LDPC_ITERATIONS, f.payload, &f.i3))
            continue;
        bool dup = false;
        for (int j = 0; j < n_dec && !dup; j++)
            dup = decoded[j].i3 == f.i3 && !memcmp(decoded[j].payload, f.payload, sizeof(f.payload));
        if (dup)
            continue;
        decoded[n_dec++] = f;
        FtxMessage out = {};
        js8_unpack(f.payload, f.i3, out.text, sizeof(out.text));
        uint8_t tones[FT8_NN];
        js8_encode(f.payload, f.i3, original, tones);
        fill_message(P, mon, cands[i], slot_start, tones, pn, out);
        cb(out, ctx);
    }
    return n_dec;
}

} // namespace

bool ftx_core_init(int sample_rate)
{
    fs = sample_rate;
    return proto_init(FTX_FT8) && proto_init(FTX_FT4);
}

// Both buffers of a protocol free (none waiting for or in the decoder).
static bool proto_idle(const Proto &P)
{
    return P.buf[0].state == FREE && P.buf[1].state == FREE;
}

void ftx_core_set_protocol(FtxProtocol p)
{
    req_protocol = p;
}

FtxProtocol ftx_core_protocol()
{
    return (FtxProtocol)req_protocol.load();
}

int ftx_core_feed(const float *x, int n, double t0, double *slot_start)
{
    const int want = req_protocol;
    if (want != protocol) {
        if (protocol != FTX_OFF)
            abandon(proto_of(protocol));
        protocol = want;
    }
    if (protocol == FTX_OFF)
        return -1;
    Proto &P = proto_of(protocol);
    // JS8: the waterfalls are set up for the speed chosen, once the decoder
    // is done with the last slot of the previous one.
    if (is_js8(protocol) && P.protocol != protocol) {
        if (!proto_idle(P))
            return -1;
        proto_init(protocol);
    }
    if (!P.ok)
        return -1;
    const int handle = 2 * proto_index(protocol);
    // Clock stepped (NTP) while filling: the slot no longer matches its samples.
    if (P.filling >= 0) {
        const double into = t0 - P.slot_start - P.time_shift;
        if (into < -0.5 || into > P.period + 1.0)
            abandon(P);
    }
    int done = -1;
    for (int i = 0; i < n; i++) {
        if (P.filling < 0) {
            const double t = t0 + (double)i / fs - P.time_shift;
            const double k = floor(t / P.period);
            if (t - k * P.period > START_WINDOW || (int64_t)k == P.last_slot)
                continue;
            P.last_slot = (int64_t)k;
            const int b = P.buf[0].state == FREE ? 0 : P.buf[1].state == FREE ? 1 : -1;
            if (b < 0) {
                skipped++;
                continue;
            }
            monitor_reset(&P.buf[b].mon);
            P.buf[b].state = FILLING;
            P.filling = b;
            P.frame_n = 0;
            P.slot_start = k * P.period;
        }
        Buffer &B = P.buf[P.filling];
        P.frame[P.frame_n++] = x[i];
        if (P.frame_n == B.mon.block_size) {
            P.frame_n = 0;
            monitor_process(&B.mon, P.frame);
            if (B.mon.wf.num_blocks >= B.mon.wf.max_blocks) {
                B.state = READY;
                done = handle + P.filling;
                *slot_start = P.slot_start;
                P.filling = -1;
            }
        }
    }
    return done;
}

int ftx_core_decode(int handle, double slot_start, FtxMessageCb cb, void *ctx)
{
    Proto &P = protos[handle / 2];
    const monitor_t &mon = P.buf[handle % 2].mon;
    const ftx_waterfall_t *wf = &mon.wf;

    EXT_RAM_BSS_ATTR static ftx_candidate_t cands[MAX_CANDIDATES];    // decoding task only
    EXT_RAM_BSS_ATTR static ftx_message_t decoded[MAX_DECODED];
    if (is_js8(P.protocol))
        return js8_decode(P, mon, slot_start, cb, ctx);
    const int n_cand = ftx_find_candidates(wf, MAX_CANDIDATES, cands, MIN_SCORE);
    const float pn = n_cand ? noise_power(*wf) : 1.0f;
    int n_dec = 0;
    for (int i = 0; i < n_cand && n_dec < MAX_DECODED; i++) {
        const ftx_candidate_t &c = cands[i];
        ftx_message_t msg;
        ftx_decode_status_t status;
        if (!ftx_decode_candidate(wf, &c, LDPC_ITERATIONS, &msg, &status))
            continue;
        bool dup = false;
        for (int j = 0; j < n_dec && !dup; j++)
            dup = decoded[j].hash == msg.hash && !memcmp(decoded[j].payload, msg.payload, sizeof(msg.payload));
        if (dup)
            continue;
        decoded[n_dec++] = msg;

        FtxMessage out = {};
        ftx_message_offsets_t offsets;
        if (ftx_message_decode(&msg, &hash_if, out.text, &offsets) != FTX_MESSAGE_RC_OK)
            continue;
        uint8_t tones[FT4_NN > FT8_NN ? FT4_NN : FT8_NN];
        if (wf->protocol == FTX_PROTOCOL_FT4)
            ft4_encode(msg.payload, tones);
        else
            ft8_encode(msg.payload, tones);
        fill_message(P, mon, c, slot_start, tones, pn, out);
        cb(out, ctx);
    }
    hash_cleanup(10);
    return n_dec;
}

void ftx_core_release(int handle)
{
    protos[handle / 2].buf[handle % 2].state = FREE;
}

int ftx_core_skipped()
{
    return skipped;
}
