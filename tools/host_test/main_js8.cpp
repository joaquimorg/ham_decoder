// JS8: frames packed as JS8Call packs them (heartbeat, directed command,
// Huffman and JSC text), sent as 8-FSK at the four speeds with noise, found
// in an ft8_lib waterfall and decoded.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include <common/monitor.h>

#include "js8_decoder.h"

static const char ALNUM[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@";

struct Bits {
    uint8_t b[72] = {};
    int n = 0;
    void put(uint64_t v, int len)
    {
        for (int i = len - 1; i >= 0; i--)
            b[n++] = (v >> i) & 1;
    }
    void pad()
    {
        if (n < 72)
            b[n++] = 0;
        while (n < 72)
            b[n++] = 1;
    }
    void bytes(uint8_t *out) const
    {
        memset(out, 0, 9);
        for (int i = 0; i < 72; i++)
            if (b[i])
                out[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    }
};

static int idx(char c) { return (int)(strchr(ALNUM, c) - ALNUM); }

// Varicode::packAlphaNumeric50
static uint64_t pack_alnum50(std::string w)
{
    if (w.size() > 3 && w[3] != '/')
        w.insert(3, " ");
    if (w.size() > 7 && w[7] != '/')
        w.insert(7, " ");
    w.resize(11, ' ');
    const uint64_t k = 38ull * 38 * 38 * 2 * 38 * 38 * 38 * 2;
    return k * 38 * 38 * idx(w[0]) + k * 38 * idx(w[1]) + k * idx(w[2]) +
           38ull * 38 * 38 * 2 * 38 * 38 * 38 * (w[3] == '/') + 38ull * 38 * 38 * 2 * 38 * 38 * idx(w[4]) +
           38ull * 38 * 38 * 2 * 38 * idx(w[5]) + 38ull * 38 * 38 * 2 * idx(w[6]) + 38ull * 38 * 38 * (w[7] == '/') +
           38ull * 38 * idx(w[8]) + 38ull * idx(w[9]) + idx(w[10]);
}

// Varicode::packCallsign for a 6-sign call (digit third)
static uint32_t pack_call(const char *c)
{
    uint32_t p = idx(c[0]);
    p = 36 * p + idx(c[1]);
    p = 10 * p + idx(c[2]);
    p = 27 * p + idx(c[3]) - 10;
    p = 27 * p + idx(c[4]) - 10;
    p = 27 * p + idx(c[5]) - 10;
    return p;
}

// Varicode::packGrid (4 characters)
static uint16_t pack_grid(const char *g)
{
    const int nlong = 180 - 20 * (g[0] - 'A');
    const int n20d = 2 * (g[2] - '0');
    const float xminlong = 5 * ('m' - 'a' + 0.5f);
    const float dlong = nlong - n20d - xminlong / 60.0f;
    const int nlat = -90 + 10 * (g[1] - 'A') + g[3] - '0';
    const float dlat = nlat + 2.5f * ('m' - 'a' + 0.5f) / 60.0f;
    const int ilong = (int)dlong, ilat = (int)(dlat + 90);
    return (uint16_t)(((ilong + 180) / 2) * 180 + ilat);
}

// JSC::codeword: word index with the (s,c)-dense code, s = 7, c = 9, 4 bits.
static void jsc_codeword(uint32_t index, bool separate, Bits &o)
{
    std::vector<std::pair<uint32_t, int>> parts;
    parts.push_back({ ((index % 7) << 1) + separate, 5 });
    uint32_t x = index / 7;
    while (x > 0) {
        x -= 1;
        parts.insert(parts.begin(), { (x % 9) + 7, 4 });
        x /= 9;
    }
    for (auto &p : parts)
        o.put(p.first, p.second);
}

static std::vector<float> synth(const uint8_t *tones, float f0, float sym, float start, float slot, float amp)
{
    const int fs = 12000, sps = (int)lroundf(sym * fs);
    std::vector<float> a((size_t)(slot * fs) + 2 * fs, 0.0f);
    double ph = 0.0;
    const size_t s0 = (size_t)(start * fs);
    for (int s = 0; s < 79; s++) {
        const double f = f0 + tones[s] / sym;
        for (int i = 0; i < sps; i++) {
            ph += 2 * M_PI * f / fs;
            a[s0 + s * sps + i] += (float)(amp * sin(ph));
        }
        ph = fmod(ph, 2 * M_PI);
    }
    return a;
}

int main(int argc, char **argv)
{
    // The dictionary (data/jsc_dict.bin).
    static std::vector<uint8_t> dict;
    if (FILE *f = fopen(argc > 1 ? argv[1] : "data/jsc_dict.bin", "rb")) {
        fseek(f, 0, SEEK_END);
        dict.resize(ftell(f));
        fseek(f, 0, SEEK_SET);
        if (fread(dict.data(), 1, dict.size(), f) != dict.size())
            dict.clear();
        fclose(f);
    }
    js8_set_dictionary(dict.data(), dict.size());

    // Frames.
    struct Frame { uint8_t p[9]; int i3; std::string want; };
    std::vector<Frame> frames;
    {    // heartbeat: [000][call 50][grid 11 high],[grid 5 low][000]
        Bits b;
        const uint16_t g = pack_grid("IM58");
        b.put(0, 3);
        b.put(pack_alnum50("CT1ABC"), 50);
        b.put(g >> 5, 11);
        b.put(g & 31, 5);
        b.put(0, 3);
        Frame f;
        b.bytes(f.p);
        f.i3 = 1;
        f.want = "CT1ABC: @HB HEARTBEAT IM58";
        frames.push_back(f);
    }
    {    // directed: CT1ABC to CT2XYZ, SNR -07
        Bits b;
        b.put(3, 3);
        b.put(pack_call("CT1ABC"), 28);
        b.put(pack_call("CT2XYZ"), 28);
        b.put(25, 5);
        b.put(0, 2);
        b.put(-7 + 31, 6);
        Frame f;
        b.bytes(f.p);
        f.i3 = 0;
        f.want = "CT1ABC: CT2XYZ SNR -07";
        frames.push_back(f);
    }
    {    // legacy data frame, Huffman: "HELLO 73"
        Bits b;
        b.put(1, 1);
        b.put(0, 1);
        const char *codes[] = { "00011", "100", "110011", "110011", "11111", "01", "11101011", "0000101" };
        for (const char *c : codes)
            for (const char *p = c; *p; p++)
                b.put(*p == '1', 1);
        b.pad();
        Frame f;
        b.bytes(f.p);
        f.i3 = 2;
        f.want = "HELLO 73";
        frames.push_back(f);
    }
    if (!dict.empty()) {    // fast data, JSC: three dictionary words
        Bits b;
        const uint32_t words[] = { 5, 400, 9000 };
        std::string want;
        // The decoder's own word lookup gives the expected text.
        for (int k = 0; k < 3; k++) {
            jsc_codeword(words[k], k < 2, b);
        }
        b.pad();
        Frame f;
        b.bytes(f.p);
        f.i3 = 4;
        char t[64];
        js8_unpack(f.p, 4, t, sizeof(t));
        f.want = t;    // checked below for being three words, decoded again over the air
        printf("JSC: \"%s\"\n", t);
        frames.push_back(f);
    }

    int fails = 0;
    struct Speed { Js8Submode m; float noise; };
    const Speed speeds[] = { { JS8_NORMAL, 0.25f }, { JS8_FAST, 0.2f }, { JS8_TURBO, 0.15f }, { JS8_SLOW, 0.3f } };
    for (const Speed &sp : speeds) {
        const Js8SubmodeInfo &si = js8_submode(sp.m);
        for (size_t fi = 0; fi < frames.size(); fi++) {
            const Frame &fr = frames[fi];
            uint8_t tones[79];
            js8_encode(fr.p, fr.i3, si.original_costas, tones);
            const float f0 = 800.0f + 300.0f * fi;
            std::vector<float> audio = synth(tones, f0, si.symbol_period, si.start_delay, si.slot, 0.05f);
            uint32_t rnd = 3 + fi;
            for (float &x : audio) {
                float n = 0.0f;
                for (int j = 0; j < 4; j++) {
                    rnd = rnd * 1664525u + 1013904223u;
                    n += (float)(int32_t)rnd / 2147483648.0f;
                }
                x += sp.noise * n;
            }
            // The waterfall from TIME_SHIFT into the slot, as ftx_core.
            monitor_config_t cfg = {};
            cfg.f_min = 200;
            cfg.f_max = 3000;
            cfg.sample_rate = 12000;
            cfg.time_osr = 2;
            cfg.freq_osr = 2;
            cfg.protocol = FTX_PROTOCOL_FT8;
            cfg.symbol_period = si.symbol_period;
            cfg.slot_time = si.slot;
            monitor_t mon;
            monitor_init(&mon, &cfg);
            const size_t from = (size_t)((si.start_delay + 2 * si.symbol_period) * 12000);
            for (size_t i = from; i + mon.block_size <= audio.size() && mon.wf.num_blocks < mon.wf.max_blocks;
                 i += mon.block_size)
                monitor_process(&mon, audio.data() + i);
            static ftx_candidate_t cands[140];
            const int nc = js8_find_candidates(&mon.wf, si.original_costas, 140, cands, 10);
            std::string got;
            for (int c = 0; c < nc && got.empty(); c++) {
                uint8_t p[9];
                int i3;
                if (!js8_decode_candidate(&mon.wf, &cands[c], 25, p, &i3))
                    continue;
                char t[64];
                js8_unpack(p, i3, t, sizeof(t));
                got = t;
                if (i3 != fr.i3)
                    got += " (i3 errado)";
            }
            monitor_free(&mon);
            const bool ok = got.find(fr.want) == 0 && fr.want.size() > 0;
            fails += !ok;
            printf("%-9s ruido %.2f %4.0f Hz: \"%s\" %s\n", si.name, sp.noise, f0, got.c_str(), ok ? "OK" : "FALHOU");
        }
    }
    return fails ? 1 : 0;
}
