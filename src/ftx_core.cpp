#include "ftx_core.h"

#include <atomic>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <common/monitor.h>
#include <ft8/decode.h>
#include <ft8/encode.h>
#include <ft8/message.h>

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

// The waterfall starts TIME_SHIFT into the slot (as in decode_ft8.c):
// transmissions start 0.5 s in, and the candidate search looks back.
constexpr double TIME_SHIFT = 0.8;
constexpr double NOMINAL_START = 0.5;
// A slot is only started within this long of its beginning (no partial slots).
constexpr double START_WINDOW = 0.25;

enum BufState { FREE = 0, FILLING = 1, READY = 2 };

struct Buffer {
    monitor_t mon;
    std::atomic<int> state{FREE};
};

struct Proto {
    bool ok = false;
    double period = 0.0;
    Buffer buf[2];
    float *frame = nullptr;       // block_size samples for monitor_process()
    int frame_n = 0;
    int filling = -1;             // buffer being filled
    int64_t last_slot = -1;       // slot index last started (or skipped)
    double slot_start = 0.0;
};

Proto protos[2];                  // FT8, FT4
std::atomic<int> req_protocol{FTX_OFF};
int protocol = FTX_OFF;
int fs = 12000;
std::atomic<int> skipped{0};

Proto &proto_of(int p)
{
    return protos[p == FTX_FT4 ? 1 : 0];
}

bool proto_init(int p)
{
    Proto &P = proto_of(p);
    monitor_config_t cfg = {};
    cfg.f_min = F_MIN;
    cfg.f_max = F_MAX;
    cfg.sample_rate = fs;
    cfg.time_osr = TIME_OSR;
    cfg.freq_osr = FREQ_OSR;
    cfg.protocol = p == FTX_FT4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
    for (Buffer &b : P.buf) {
        monitor_init(&b.mon, &cfg);
        if (!b.mon.wf.mag || !b.mon.window || !b.mon.last_frame || !b.mon.timedata ||
            !b.mon.freqdata || !b.mon.fft_work)
            return false;
    }
    P.frame = (float *)malloc(P.buf[0].mon.block_size * sizeof(float));
    P.period = p == FTX_FT4 ? FT4_SLOT_TIME : FT8_SLOT_TIME;
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
HashEntry hash_table[HASH_SIZE];

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
    static uint32_t hist[256];
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
float estimate_snr(const monitor_t &mon, const ftx_candidate_t &c, const ftx_message_t &msg, float pn)
{
    const ftx_waterfall_t &wf = mon.wf;
    const bool ft4 = wf.protocol == FTX_PROTOCOL_FT4;
    uint8_t tones[FT4_NN > FT8_NN ? FT4_NN : FT8_NN];
    int n_sym, n_tones;
    if (ft4) {
        ft4_encode(msg.payload, tones);
        n_sym = FT4_NN;
        n_tones = 4;
    } else {
        ft8_encode(msg.payload, tones);
        n_sym = FT8_NN;
        n_tones = 8;
    }
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

} // namespace

bool ftx_core_init(int sample_rate)
{
    fs = sample_rate;
    return proto_init(FTX_FT8) && proto_init(FTX_FT4);
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
    if (!P.ok)
        return -1;
    const int handle = (protocol == FTX_FT4 ? 2 : 0);
    // Clock stepped (NTP) while filling: the slot no longer matches its samples.
    if (P.filling >= 0) {
        const double into = t0 - P.slot_start - TIME_SHIFT;
        if (into < -0.5 || into > P.period + 1.0)
            abandon(P);
    }
    int done = -1;
    for (int i = 0; i < n; i++) {
        if (P.filling < 0) {
            const double t = t0 + (double)i / fs - TIME_SHIFT;
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

    static ftx_candidate_t cands[MAX_CANDIDATES];    // decoding task only
    static ftx_message_t decoded[MAX_DECODED];
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
        const float time_s = (c.time_offset + (float)c.time_sub / wf->time_osr) * mon.symbol_period;
        out.slot_start = slot_start;
        out.freq_hz = (mon.min_bin + c.freq_offset + (float)c.freq_sub / wf->freq_osr) / mon.symbol_period;
        // The candidate time is one symbol late (analysis window alignment).
        out.dt = time_s + (float)(TIME_SHIFT - NOMINAL_START) - mon.symbol_period;
        out.snr_db = estimate_snr(mon, c, msg, pn);
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
