#include "js8_decoder.h"

// Written from the JS8Call sources (frame layout, LDPC code, CRC, packing)
// and modelled on ft8_lib's FT8 decoder (candidate search, soft bits, belief
// propagation). Validated on a PC with tools/host_test (js8).
#pragma GCC optimize("O2")

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

namespace {

#include "js8_ldpc.inc"

constexpr int NN = 79, ND = 58, NS = 7;
constexpr int N = 174, K = 87, M = 87;

const uint8_t COSTAS_ORIGINAL[NS] = { 4, 2, 5, 6, 1, 3, 0 };
const uint8_t COSTAS_A[NS] = { 0, 6, 2, 3, 5, 4, 1 };
const uint8_t COSTAS_B[NS] = { 1, 5, 0, 2, 3, 6, 4 };
const uint8_t COSTAS_C[NS] = { 2, 5, 0, 6, 4, 1, 3 };

const Js8SubmodeInfo SUBMODES[] = {
    { "JS8", 1920 / 12000.0f, 15.0f, 0.5f, true },
    { "JS8 FAST", 1200 / 12000.0f, 10.0f, 0.2f, false },
    { "JS8 TURBO", 600 / 12000.0f, 6.0f, 0.1f, false },
    { "JS8 SLOW", 3840 / 12000.0f, 30.0f, 0.5f, false },
};

const uint8_t *costas(bool original, int block)
{
    if (original)
        return COSTAS_ORIGINAL;
    return block == 0 ? COSTAS_A : block == 1 ? COSTAS_B : COSTAS_C;
}

const WF_ELEM_T *cand_mag(const ftx_waterfall_t *wf, const ftx_candidate_t *c)
{
    int offset = c->time_offset;
    offset = offset * wf->time_osr + c->time_sub;
    offset = offset * wf->freq_osr + c->freq_sub;
    offset = offset * wf->num_bins + c->freq_offset;
    return wf->mag + offset;
}

// As ft8_lib's ft8_sync_score(): the expected Costas tone against its
// neighbours in frequency and time.
int sync_score(const ftx_waterfall_t *wf, const ftx_candidate_t *c, bool original)
{
    int score = 0, num = 0;
    const WF_ELEM_T *mag = cand_mag(wf, c);
    for (int m = 0; m < 3; m++) {
        const uint8_t *cs = costas(original, m);
        for (int k = 0; k < NS; k++) {
            const int block = 36 * m + k;
            const int abs_block = c->time_offset + block;
            if (abs_block < 0)
                continue;
            if (abs_block >= wf->num_blocks)
                break;
            const WF_ELEM_T *p = mag + block * wf->block_stride;
            const int sm = cs[k];
            if (sm > 0) {
                score += WF_ELEM_MAG_INT(p[sm]) - WF_ELEM_MAG_INT(p[sm - 1]);
                num++;
            }
            if (sm < 7) {
                score += WF_ELEM_MAG_INT(p[sm]) - WF_ELEM_MAG_INT(p[sm + 1]);
                num++;
            }
            if (k > 0 && abs_block > 0) {
                score += WF_ELEM_MAG_INT(p[sm]) - WF_ELEM_MAG_INT(p[sm - wf->block_stride]);
                num++;
            }
            if (k + 1 < NS && abs_block + 1 < wf->num_blocks) {
                score += WF_ELEM_MAG_INT(p[sm]) - WF_ELEM_MAG_INT(p[sm + wf->block_stride]);
                num++;
            }
        }
    }
    return num ? score / num : 0;
}

// Min-heap on the score (the worst candidate on top).
void heap_down(ftx_candidate_t *h, int n, int i)
{
    while (true) {
        int s = i;
        const int l = 2 * i + 1, r = l + 1;
        if (l < n && h[l].score < h[s].score)
            s = l;
        if (r < n && h[r].score < h[s].score)
            s = r;
        if (s == i)
            return;
        const ftx_candidate_t t = h[i];
        h[i] = h[s];
        h[s] = t;
        i = s;
    }
}

void heap_up(ftx_candidate_t *h, int i)
{
    while (i > 0) {
        const int p = (i - 1) / 2;
        if (h[p].score <= h[i].score)
            return;
        const ftx_candidate_t t = h[i];
        h[i] = h[p];
        h[p] = t;
        i = p;
    }
}

// Soft bits of one data symbol: no Gray code (the tone number is the 3 bits).
void extract_symbol(const WF_ELEM_T *p, float *logl)
{
    float s[8];
    for (int j = 0; j < 8; j++)
        s[j] = WF_ELEM_MAG(p[j]);
    logl[0] = fmaxf(fmaxf(s[4], s[5]), fmaxf(s[6], s[7])) - fmaxf(fmaxf(s[0], s[1]), fmaxf(s[2], s[3]));
    logl[1] = fmaxf(fmaxf(s[2], s[3]), fmaxf(s[6], s[7])) - fmaxf(fmaxf(s[0], s[1]), fmaxf(s[4], s[5]));
    logl[2] = fmaxf(fmaxf(s[1], s[3]), fmaxf(s[5], s[7])) - fmaxf(fmaxf(s[0], s[2]), fmaxf(s[4], s[6]));
}

inline float fast_tanh(float x)
{
    if (x < -4.97f)
        return -1.0f;
    if (x > 4.97f)
        return 1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline float fast_atanh(float x)
{
    const float x2 = x * x;
    return x * (945.0f - 735.0f * x2 + 64.0f * x2 * x2) / (945.0f - 1050.0f * x2 + 225.0f * x2 * x2);
}

int ldpc_errors(const uint8_t *plain)
{
    int errors = 0;
    for (int m = 0; m < M; m++) {
        int x = 0;
        for (int i = 0; i < JS8_NRW[m]; i++)
            x ^= plain[JS8_NM[m][i]];
        errors += x;
    }
    return errors;
}

// Belief propagation (log domain), as ft8_lib's bp_decode() with the JS8 code.
int bp_decode(const float *llr, int max_iters, uint8_t *plain)
{
    EXT_RAM_BSS_ATTR static float tov[N][3];    // decoding task only; the internal RAM is short
    EXT_RAM_BSS_ATTR static float toc[M][7];
    memset(tov, 0, sizeof(tov));
    int min_errors = M;
    for (int iter = 0; iter < max_iters; iter++) {
        int ones = 0;
        for (int n = 0; n < N; n++) {
            plain[n] = llr[n] + tov[n][0] + tov[n][1] + tov[n][2] > 0.0f;
            ones += plain[n];
        }
        if (ones == 0)
            break;    // all zeros: not a valid frame
        const int errors = ldpc_errors(plain);
        if (errors < min_errors) {
            min_errors = errors;
            if (errors == 0)
                break;
        }
        for (int m = 0; m < M; m++) {
            for (int i = 0; i < JS8_NRW[m]; i++) {
                const int n = JS8_NM[m][i];
                float t = llr[n];
                for (int k = 0; k < 3; k++)
                    if (JS8_MN[n][k] != m)
                        t += tov[n][k];
                toc[m][i] = fast_tanh(-t / 2.0f);
            }
        }
        for (int n = 0; n < N; n++) {
            for (int k = 0; k < 3; k++) {
                const int m = JS8_MN[n][k];
                float t = 1.0f;
                for (int i = 0; i < JS8_NRW[m]; i++)
                    if (JS8_NM[m][i] != n)
                        t *= toc[m][i];
                tov[n][k] = -2.0f * fast_atanh(t);
            }
        }
    }
    return min_errors;
}

// CRC-12 (polynomial 0xC06) of the frame, as JS8Call's boost::augmented_crc
// over 11 bytes (75 bits and zeros), XOR 42.
uint16_t crc12(const uint8_t *bits75)
{
    uint8_t bytes[10] = {};
    for (int i = 0; i < 75; i++)
        if (bits75[i])
            bytes[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    // The augmented CRC over n bits with 12 zero bits at the end equals the
    // direct one over the first n - 12 (76 here).
    uint16_t rem = 0;
    for (int i = 0; i < 76; i++) {
        if (i % 8 == 0)
            rem ^= (uint16_t)(bytes[i / 8] << 4);
        rem = (rem & 0x800) ? (uint16_t)((rem << 1) ^ 0xC06) : (uint16_t)(rem << 1);
    }
    return (uint16_t)((rem & 0xFFF) ^ 42);
}

// ---------------------------------------------------------------------------
// Frame text (Varicode::unpack* and DecodedText of JS8Call).

const char ALPHABET72[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+/?.";
const char ALNUM[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@";    // 39 signs

constexpr uint32_t NBASECALL = 37u * 36 * 10 * 27 * 27 * 27;
constexpr uint32_t NBASEGRID = 180 * 180;
constexpr uint32_t NUSERGRID = NBASEGRID + 10;
constexpr uint32_t NMAXGRID = (1 << 15) - 1;

const char *const GROUPS[] = {
    "<....>", "@ALLCALL", "@JS8NET", "@DX/NA", "@DX/SA", "@DX/EU", "@DX/AS", "@DX/AF", "@DX/OC", "@DX/AN",
    "@REGION/1", "@REGION/2", "@REGION/3", "@GROUP/0", "@GROUP/1", "@GROUP/2", "@GROUP/3", "@GROUP/4",
    "@GROUP/5", "@GROUP/6", "@GROUP/7", "@GROUP/8", "@GROUP/9", "@COMMAND", "@CONTROL", "@NET", "@NTS",
    "@RESERVE/0", "@RESERVE/1", "@RESERVE/2", "@RESERVE/3", "@RESERVE/4", "@APRSIS", "@RAGCHEW", "@JS8",
    "@EMCOMM", "@ARES", "@MARS", "@AMRRON", "@RACES", "@RAYNET", "@RADAR", "@SKYWARN", "@CQ", "@HB", "@QSO",
    "@QSOPARTY", "@CONTEST", "@FIELDDAY", "@SOTA", "@IOTA", "@POTA", "@QRP", "@QRO",
};    // NBASECALL + 1 ..

const char *const CQS[8] = { "CQ CQ CQ", "CQ DX", "CQ QRP", "CQ CONTEST", "CQ FIELD", "CQ FD", "CQ CQ", "CQ" };

// Directed commands by number: of the names JS8Call maps to the same number,
// the one its table lists first in sort order (what QMap::key() gives).
const char *const CMDS[32] = {
    " SNR?", " DIT DIT", " NACK", " HEARING?", " GRID?", ">", " STATUS?", " STATUS", " HEARING", " MSG",
    " MSG TO:", " QUERY", " QUERY MSGS", " QUERY CALL", " ACK", " GRID", " INFO?", " INFO", " FB", " HW CPY?",
    " SK", " RR", " QSL?", " QSL", " CMD", " SNR", " NO", " YES", " 73", " HEARTBEAT SNR", " AGN?", " ",
};

bool snr_cmd(int cmd) { return cmd == 25 || cmd == 29; }

// Huffman code of the legacy data frames.
struct Huff { const char *code; char c; };
const Huff HUFF[] = {
    { "01", ' ' }, { "100", 'E' }, { "1101", 'T' }, { "0011", 'A' }, { "11111", 'O' }, { "11100", 'I' },
    { "10111", 'N' }, { "10100", 'S' }, { "00011", 'H' }, { "00000", 'R' }, { "111011", 'D' },
    { "110011", 'L' }, { "110001", 'C' }, { "101101", 'U' }, { "101011", 'M' }, { "001011", 'W' },
    { "001001", 'F' }, { "000101", 'G' }, { "000011", 'Y' }, { "1111011", 'P' }, { "1111001", 'B' },
    { "1110100", '.' }, { "1100101", 'V' }, { "1100100", 'K' }, { "1100001", '-' }, { "1100000", '+' },
    { "1011001", '?' }, { "1011000", '!' }, { "1010101", '"' }, { "1010100", 'X' }, { "0010101", '0' },
    { "0010100", 'J' }, { "0010001", '1' }, { "0010000", 'Q' }, { "0001001", '2' }, { "0001000", 'Z' },
    { "0000101", '3' }, { "0000100", '5' }, { "11110101", '4' }, { "11110100", '9' }, { "11110001", '8' },
    { "11110000", '6' }, { "11101011", '7' }, { "11101010", '/' },
};

const uint8_t *dict = nullptr;
size_t dict_size = 0;
constexpr uint32_t JSC_WORDS = 262144, JSC_GROUP = 64;

const char *jsc_word(uint32_t i)
{
    if (!dict || i >= JSC_WORDS)
        return nullptr;
    const uint8_t *g = dict + 4 * (i / JSC_GROUP);
    const uint32_t off = g[0] | (g[1] << 8) | (g[2] << 16) | ((uint32_t)g[3] << 24);
    const char *p = (const char *)dict + 4 * (JSC_WORDS / JSC_GROUP) + off;
    for (uint32_t k = i % JSC_GROUP; k > 0; k--)
        p += strlen(p) + 1;
    return p;
}

struct Out {
    char *s;
    size_t size, len;
    void add(const char *t)
    {
        while (*t && len + 1 < size)
            s[len++] = *t++;
        s[len] = 0;
    }
    void addc(char c)
    {
        const char t[2] = { c, 0 };
        add(t);
    }
};

uint64_t bits_value(const uint8_t *bits, int from, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | bits[from + i];
    return v;
}

void unpack_callsign(uint32_t v, bool portable, Out &o)
{
    if (v > NBASECALL && v - NBASECALL <= sizeof(GROUPS) / sizeof(GROUPS[0])) {
        o.add(GROUPS[v - NBASECALL - 1]);
        return;
    }
    char w[8];
    w[5] = ALNUM[v % 27 + 10]; v /= 27;
    w[4] = ALNUM[v % 27 + 10]; v /= 27;
    w[3] = ALNUM[v % 27 + 10]; v /= 27;
    w[2] = ALNUM[v % 10]; v /= 10;
    w[1] = ALNUM[v % 36]; v /= 36;
    w[0] = v < 39 ? ALNUM[v] : '?';
    w[6] = 0;
    char call[12];
    if (!strncmp(w, "3D0", 3))
        snprintf(call, sizeof(call), "3DA0%s", w + 3);
    else if (w[0] == 'Q' && w[1] >= 'A' && w[1] <= 'Z')
        snprintf(call, sizeof(call), "3X%s", w + 1);
    else
        strcpy(call, w);
    // Trimmed (the alphabet's space pads short calls).
    char *a = call;
    while (*a == ' ')
        a++;
    char *e = a + strlen(a);
    while (e > a && e[-1] == ' ')
        *--e = 0;
    o.add(a);
    if (portable)
        o.add("/P");
}

void unpack_alnum50(uint64_t v, Out &o)
{
    char w[12];
    w[10] = ALNUM[v % 38]; v /= 38;
    w[9] = ALNUM[v % 38]; v /= 38;
    w[8] = ALNUM[v % 38]; v /= 38;
    w[7] = v % 2 ? '/' : ' '; v /= 2;
    w[6] = ALNUM[v % 38]; v /= 38;
    w[5] = ALNUM[v % 38]; v /= 38;
    w[4] = ALNUM[v % 38]; v /= 38;
    w[3] = v % 2 ? '/' : ' '; v /= 2;
    w[2] = ALNUM[v % 38]; v /= 38;
    w[1] = ALNUM[v % 38]; v /= 38;
    w[0] = ALNUM[v % 39];
    for (int i = 0; i < 11; i++)
        if (w[i] != ' ')
            o.addc(w[i]);
}

void unpack_grid(uint32_t v, Out &o)
{
    if (v > NBASEGRID)
        return;
    const float dlat = (float)(v % 180) - 90.0f;
    float dlong = (float)(v / 180 * 2) - 180.0f + 2.0f;
    if (dlong < -180) dlong += 360;
    if (dlong > 180) dlong -= 360;
    const int nlong = (int)(60.0f * (180.0f - dlong) / 5.0f);
    const int nlat = (int)(60.0f * (dlat + 90.0f) / 2.5f);
    const char g[5] = { (char)('A' + nlong / 240), (char)('A' + nlat / 240),
                        (char)('0' + (nlong - 240 * (nlong / 240)) / 24),
                        (char)('0' + (nlat - 240 * (nlat / 240)) / 24), 0 };
    o.add(g);
}

void format_snr(int snr, Out &o)
{
    if (snr < -60 || snr > 60)
        return;
    char t[8];
    snprintf(t, sizeof(t), snr < 0 ? "-%02d" : "+%02d", snr < 0 ? -snr : snr);
    o.add(t);
}

// JSC: (s,c)-dense code of word indices, s = 7, c = 9, 4-bit digits.
void jsc_decompress(const uint8_t *bits, int n, Out &o)
{
    constexpr uint32_t S = 7, C = 9;
    uint32_t base[8];
    base[0] = 0;
    uint32_t sc = S;
    for (int k = 1; k < 8; k++) {
        base[k] = base[k - 1] + sc;
        sc *= C;
    }
    uint32_t digits[32];
    bool sep[32] = {};
    int nd = 0;
    for (int i = 0; i + 4 <= n && nd < 32;) {
        const uint32_t d = (uint32_t)bits_value(bits, i, 4);
        digits[nd] = d;
        i += 4;
        if (d < S) {
            if (i < n && bits[i])
                sep[nd] = true;
            i++;
        }
        nd++;
    }
    for (int start = 0; start < nd;) {
        uint32_t j = 0;
        int k = 0;
        while (start + k < nd && digits[start + k] >= S) {
            j = j * C + (digits[start + k] - S);
            k++;
        }
        if (j >= JSC_WORDS || start + k >= nd)
            break;
        j = j * S + digits[start + k] + base[k];
        if (j >= JSC_WORDS)
            break;
        const char *w = jsc_word(j);
        if (!w) {
            o.add("<JSC>");
            return;
        }
        o.add(w);
        if (sep[start + k])
            o.addc(' ');
        start += k + 1;
    }
}

void huff_decode(const uint8_t *bits, int n, Out &o)
{
    int i = 0;
    while (i < n) {
        bool found = false;
        for (const Huff &h : HUFF) {
            const int len = (int)strlen(h.code);
            if (i + len > n)
                continue;
            bool match = true;
            for (int k = 0; k < len && match; k++)
                match = bits[i + k] == (h.code[k] == '1');
            if (match) {
                o.addc(h.c);
                i += len;
                found = true;
                break;
            }
        }
        if (!found)
            break;
    }
}

int last_zero(const uint8_t *bits, int n)
{
    for (int i = n - 1; i >= 0; i--)
        if (!bits[i])
            return i;
    return -1;
}

}    // namespace

const Js8SubmodeInfo &js8_submode(Js8Submode m)
{
    return SUBMODES[m >= JS8_NORMAL && m <= JS8_SLOW ? m : JS8_NORMAL];
}

void js8_set_dictionary(const uint8_t *blob, size_t size)
{
    dict = size > 4 * (JSC_WORDS / JSC_GROUP) ? blob : nullptr;
    dict_size = size;
}

int js8_find_candidates(const ftx_waterfall_t *wf, bool original, int max, ftx_candidate_t *heap, int min_score)
{
    int n = 0;
    ftx_candidate_t c;
    for (c.time_sub = 0; c.time_sub < wf->time_osr; c.time_sub++) {
        for (c.freq_sub = 0; c.freq_sub < wf->freq_osr; c.freq_sub++) {
            for (c.time_offset = -10; c.time_offset < 20; c.time_offset++) {
                for (c.freq_offset = 0; c.freq_offset + 7 < wf->num_bins; c.freq_offset++) {
                    c.score = (int16_t)sync_score(wf, &c, original);
                    if (c.score < min_score)
                        continue;
                    if (n == max) {
                        if (c.score <= heap[0].score)
                            continue;
                        heap[0] = c;
                        heap_down(heap, n, 0);
                    } else {
                        heap[n] = c;
                        heap_up(heap, n);
                        n++;
                    }
                }
            }
        }
    }
    // Best first.
    for (int len = n; len > 1; len--) {
        const ftx_candidate_t t = heap[len - 1];
        heap[len - 1] = heap[0];
        heap[0] = t;
        heap_down(heap, len - 1, 0);
    }
    return n;
}

bool js8_decode_candidate(const ftx_waterfall_t *wf, const ftx_candidate_t *cand, int max_iterations,
                          uint8_t payload[9], int *i3)
{
    float llr[N];
    const WF_ELEM_T *mag = cand_mag(wf, cand);
    for (int k = 0; k < ND; k++) {
        const int sym = k + (k < 29 ? 7 : 14);
        const int block = cand->time_offset + sym;
        if (block < 0 || block >= wf->num_blocks)
            llr[3 * k] = llr[3 * k + 1] = llr[3 * k + 2] = 0.0f;
        else
            extract_symbol(mag + sym * wf->block_stride, llr + 3 * k);
    }
    // Normalised as ft8_lib does.
    float sum = 0.0f, sum2 = 0.0f;
    for (int i = 0; i < N; i++) {
        sum += llr[i];
        sum2 += llr[i] * llr[i];
    }
    const float var = (sum2 - sum * sum / N) / N;
    if (var <= 0.0f)
        return false;
    const float norm = sqrtf(24.0f / var);
    for (int i = 0; i < N; i++)
        llr[i] *= norm;

    uint8_t plain[N];
    if (bp_decode(llr, max_iterations, plain) != 0)
        return false;
    uint8_t msg[K];
    for (int k = 0; k < K; k++)
        msg[k] = plain[JS8_COLORDER[M + k]];
    const uint16_t got = (uint16_t)bits_value(msg, 75, 12);
    if (crc12(msg) != got)
        return false;
    memset(payload, 0, 9);
    for (int i = 0; i < 72; i++)
        if (msg[i])
            payload[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    *i3 = (int)bits_value(msg, 72, 3);
    return true;
}

void js8_encode(const uint8_t payload[9], int i3, bool original, uint8_t tones[NN])
{
    uint8_t msg[K];
    for (int i = 0; i < 72; i++)
        msg[i] = (payload[i / 8] >> (7 - i % 8)) & 1;
    for (int i = 0; i < 3; i++)
        msg[72 + i] = (i3 >> (2 - i)) & 1;
    const uint16_t crc = crc12(msg);
    for (int i = 0; i < 12; i++)
        msg[75 + i] = (crc >> (11 - i)) & 1;
    uint8_t tmp[N], cw[N];
    for (int i = 0; i < M; i++) {
        int x = 0;
        for (int j = 0; j < K; j++)
            if ((JS8_GEN[i][j / 8] >> (7 - j % 8)) & 1)
                x ^= msg[j];
        tmp[i] = (uint8_t)x;
    }
    memcpy(tmp + M, msg, K);
    for (int i = 0; i < N; i++)
        cw[JS8_COLORDER[i]] = tmp[i];
    for (int m = 0; m < 3; m++)
        memcpy(tones + 36 * m, costas(original, m), NS);
    for (int k = 0; k < ND; k++)
        tones[k + (k < 29 ? 7 : 14)] = (uint8_t)(cw[3 * k] * 4 + cw[3 * k + 1] * 2 + cw[3 * k + 2]);
}

void js8_unpack(const uint8_t payload[9], int i3, char *out, size_t size)
{
    Out o = { out, size, 0 };
    out[0] = 0;
    uint8_t bits[72];
    for (int i = 0; i < 72; i++)
        bits[i] = (payload[i / 8] >> (7 - i % 8)) & 1;
    const bool fast_data = i3 & 4;
    const int type = (int)bits_value(bits, 0, 3);

    if (fast_data) {
        // Compressed text over the whole frame, padded with 0 then 1s.
        const int n = last_zero(bits, 72);
        if (n > 0)
            jsc_decompress(bits, n, o);
    } else if (bits[0]) {
        // Legacy data frame: [1][compressed?][text...][0][1...].
        const int n = last_zero(bits + 1, 71);
        if (n > 1) {
            if (bits[1])
                jsc_decompress(bits + 2, n - 1, o);
            else
                huff_decode(bits + 2, n - 1, o);
        }
    } else if (type == 0 || type == 1 || type == 2) {
        // [3 type][50 callsign][11 extra high],[5 extra low][3 bits]
        const uint8_t rem = (uint8_t)bits_value(bits, 64, 8);
        const uint32_t num = ((uint32_t)bits_value(bits, 53, 11) << 5) | (rem >> 3);
        const int bits3 = rem & 7;
        unpack_alnum50(bits_value(bits, 3, 50), o);
        if (type == 0) {
            o.add(": ");
            if (num & 0x8000) {
                o.add("@ALLCALL ");
                o.add(CQS[bits3]);
            } else {
                o.add("@HB HEARTBEAT");
            }
            o.addc(' ');
            unpack_grid(num & 0x7FFF, o);
        } else {
            if (type == 1)
                o.add(": ");
            if (num <= NBASEGRID) {
                o.addc(' ');
                unpack_grid(num, o);
            } else if (num >= NUSERGRID && num < NMAXGRID) {
                const uint32_t v = num - NUSERGRID;
                const int cmd = (v & 0x80) ? ((v & 0x40) ? 29 : 25) : (int)(v & 0x7F);
                o.add(cmd < 32 ? CMDS[cmd] : "");
                if (snr_cmd(cmd)) {
                    o.addc(' ');
                    format_snr((int)(v & 0x3F) - 31, o);
                }
            }
        }
    } else if (type == 3) {
        // [3][28 from][28 to][5 cmd],[from /P][to /P][6 number]
        const uint8_t rem = (uint8_t)bits_value(bits, 64, 8);
        const int cmd = (int)bits_value(bits, 59, 5);
        const int extra = rem % 64;
        unpack_callsign((uint32_t)bits_value(bits, 3, 28), rem & 0x80, o);
        o.add(": ");
        unpack_callsign((uint32_t)bits_value(bits, 31, 28), rem & 0x40, o);
        o.add(CMDS[cmd]);
        if (extra) {
            o.addc(' ');
            if (snr_cmd(cmd)) {
                format_snr(extra - 31, o);
            } else {
                char t[8];
                snprintf(t, sizeof(t), "%d", extra - 31);
                o.add(t);
            }
        }
    }
    if (!o.len) {
        // Not understood: the raw frame, as JS8Call shows it.
        for (int i = 0; i < 12; i++)
            o.addc(ALPHABET72[bits_value(bits, 6 * i, 6) % 66]);
    }
}
