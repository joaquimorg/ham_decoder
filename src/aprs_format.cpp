#include "aprs_format.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct Out {
    char *p;
    size_t size;
    int n = 0;

    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        if (n >= (int)size - 1)
            return;
        va_list ap;
        va_start(ap, fmt);
        const int k = vsnprintf(p + n, size - n, fmt, ap);
        va_end(ap);
        if (k > 0)
            n = n + k < (int)size - 1 ? n + k : (int)size - 1;
    }
    // Printable characters of [s, s + len); the rest as '.'.
    void raw(const char *s, int len)
    {
        for (int i = 0; i < len && n < (int)size - 1; i++) {
            const unsigned char c = (unsigned char)s[i];
            p[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        p[n] = 0;
    }
};

bool en_lang = false;
const char *T(const char *pt, const char *en) { return en_lang ? en : pt; }

int format_call(const uint8_t *a, char *out)
{
    int k = 0;
    for (int i = 0; i < 6; i++) {
        const char c = (char)(a[i] >> 1);
        if (c != ' ')
            out[k++] = c;
    }
    const int ssid = (a[6] >> 1) & 0x0F;
    if (ssid)
        k += sprintf(out + k, "-%d", ssid);
    out[k] = 0;
    return k;
}

// ---------------------------------------------------------------------------
// Symbols: the common ones named; the rest as their two characters.

struct Sym {
    char table;    // '/' primary, '\\' alternate (also overlays)
    char code;
    const char *pt, *en;
};
const Sym SYMBOLS[] = {
    { '/', '!', "polícia", "police" },        { '/', '#', "digipeater", "digipeater" },
    { '/', '&', "gateway HF", "HF gateway" }, { '/', '-', "casa", "house" },
    { '/', '<', "mota", "motorcycle" },       { '/', '>', "carro", "car" },
    { '/', 'O', "balão", "balloon" },         { '/', 'R', "autocaravana", "RV" },
    { '/', 'Y', "veleiro", "yacht" },         { '/', '[', "pessoa", "person" },
    { '/', '^', "avião", "aircraft" },        { '/', '_', "meteo", "weather" },
    { '/', '`', "parabólica", "dish" },       { '/', 'a', "ambulância", "ambulance" },
    { '/', 'b', "bicicleta", "bicycle" },     { '/', 'f', "bombeiros", "fire truck" },
    { '/', 'j', "jipe", "jeep" },             { '/', 'k', "camião", "truck" },
    { '/', 'r', "repetidor", "repeater" },    { '/', 's', "barco", "boat" },
    { '/', 'u', "camião", "truck" },          { '/', 'v', "carrinha", "van" },
    { '/', 'y', "casa c/ yagi", "yagi" },     { '\\', '#', "digipeater", "digipeater" },
    { '\\', '&', "gateway", "gateway" },      { '\\', '-', "casa", "house" },
    { '\\', '>', "carro", "car" },            { '\\', '_', "meteo", "weather" },
    { '\\', 'a', "ARES", "ARES" },            { '\\', 'n', "nó", "node" },
};

void add_symbol(Out &o, char table, char code)
{
    const char t = table == '/' ? '/' : '\\';
    for (const Sym &s : SYMBOLS) {
        if (s.table == t && s.code == code) {
            if (t == '\\' && table != '\\')
                o.add(" · %s (%c)", T(s.pt, s.en), table);    // overlay character
            else
                o.add(" · %s", T(s.pt, s.en));
            return;
        }
    }
    o.add(" · %s %c%c", T("símbolo", "symbol"), table, code);
}

// ---------------------------------------------------------------------------
// Positions.

struct Pos {
    bool ok = false;
    double lat = 0, lon = 0;
    char table = 0, code = 0;
    int course = -1;          // degrees
    float speed_kmh = -1;
    float alt_m = -1000;
    int used = 0;             // characters taken from the information field
};

bool digits(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i]) && s[i] != ' ')
            return false;
    return true;
}

double num(const char *s, int n)
{
    char b[16];
    if (n > 15)
        n = 15;
    for (int i = 0; i < n; i++)
        b[i] = s[i] == ' ' ? '0' : s[i];    // position ambiguity
    b[n] = 0;
    return atof(b);
}

int b91(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++)
        v = v * 91 + ((unsigned char)s[i] - 33);
    return v;
}

// "DDMM.mmN/DDDMM.mmW>" or compressed "/YYYYXXXX>csT".
Pos parse_position(const char *s, int len)
{
    Pos p;
    if (len >= 19 && digits(s, 4) && s[4] == '.' && (s[7] == 'N' || s[7] == 'S') && digits(s + 9, 5) &&
        s[14] == '.' && (s[17] == 'E' || s[17] == 'W')) {
        p.lat = num(s, 2) + num(s + 2, 5) / 60.0;
        if (s[7] == 'S')
            p.lat = -p.lat;
        p.lon = num(s + 9, 3) + num(s + 12, 5) / 60.0;
        if (s[17] == 'W')
            p.lon = -p.lon;
        p.table = s[8];
        p.code = s[18];
        p.used = 19;
        p.ok = true;
        // Course/speed extension "ddd/sss" (knots), except for weather, where
        // it is the wind.
        if (p.code != '_' && len >= 26 && digits(s + 19, 3) && s[22] == '/' && digits(s + 23, 3)) {
            p.course = (int)num(s + 19, 3);
            p.speed_kmh = (float)(num(s + 23, 3) * 1.852);
            p.used = 26;
        }
        return p;
    }
    if (len >= 13 && (s[0] == '/' || s[0] == '\\' || isupper((unsigned char)s[0]) || (s[0] >= 'a' && s[0] <= 'j'))) {
        for (int i = 1; i < 9; i++)
            if ((unsigned char)s[i] < 33 || (unsigned char)s[i] > 123)
                return p;
        p.lat = 90.0 - b91(s + 1, 4) / 380926.0;
        p.lon = -180.0 + b91(s + 5, 4) / 190463.0;
        p.table = s[0];
        p.code = s[9];
        p.used = 13;
        p.ok = true;
        const char c = s[10], sp = s[11], t = s[12];
        if (c != ' ') {
            const int cc = c - 33, ss = sp - 33, type = t - 33;
            if (((type >> 3) & 3) == 2) {
                p.alt_m = (float)(pow(1.002, cc * 91 + ss) * 0.3048);
            } else if (cc >= 0 && cc <= 89) {
                p.course = cc * 4;
                p.speed_kmh = (float)((pow(1.08, ss) - 1.0) * 1.852);
            }
        }
        return p;
    }
    return p;
}

void add_position(Out &o, const Pos &p)
{
    o.add("%.4f%c %.4f%c", fabs(p.lat), p.lat >= 0 ? 'N' : 'S', fabs(p.lon), p.lon >= 0 ? 'E' : 'W');
    add_symbol(o, p.table, p.code);
    if (p.course >= 0 && p.speed_kmh >= 0 && (p.course || p.speed_kmh > 0.5f))
        o.add(" · %d° %.0f km/h", p.course, p.speed_kmh);
    if (p.alt_m > -1000)
        o.add(" · alt %.0f m", p.alt_m);
}

// Comment: "/A=nnnnnn" (feet) becomes the altitude; the rest is shown.
void add_comment(Out &o, const char *s, int len, bool have_alt)
{
    while (len > 0 && *s == ' ') {
        s++;
        len--;
    }
    const char *a = nullptr;
    for (int i = 0; i + 9 <= len; i++) {
        if (s[i] == '/' && s[i + 1] == 'A' && s[i + 2] == '=' && (digits(s + i + 3, 6) || s[i + 3] == '-')) {
            a = s + i;
            break;
        }
    }
    if (a && !have_alt)
        o.add(" · alt %.0f m", num(a + 3, 6) * 0.3048);
    if (a) {
        const int pre = (int)(a - s);
        if (pre > 0) {
            o.add(" · ");
            o.raw(s, pre);
        }
        if (len - pre - 9 > 0) {
            o.add("%s", pre > 0 ? " " : " · ");
            o.raw(a + 9, len - pre - 9);
        }
    } else if (len > 0) {
        o.add(" · ");
        o.raw(s, len);
    }
}

// Weather fields ("_ddd/sss" was taken as wind by the caller). Returns the
// characters used.
int add_weather(Out &o, const char *s, int len, int wind_dir, int wind_mph)
{
    bool first = true;
    auto sep = [&]() {
        if (first)
            o.add(" %s:", T("meteo", "weather"));
        else
            o.add(",");
        first = false;
    };
    auto field = [&](int i, int n, double &v) {
        if (i + 1 + n > len)
            return false;
        for (int k = 0; k < n; k++) {
            const char c = s[i + 1 + k];
            if (!isdigit((unsigned char)c) && c != '-' && c != '.' && c != ' ')
                return false;
        }
        v = num(s + i + 1, n);
        return true;
    };
    int i = 0;
    double t = 0, g = -1, h = -1, b = -1, r = -1, p24 = -1, P = -1;
    bool have_t = false;
    while (i < len) {
        double v;
        const char c = s[i];
        int n = c == 'h' ? 2 : c == 'b' ? 5 : 3;
        if (!strchr("gtrpPhbLlsc#", c) || !field(i, n, v))
            break;
        if (s[i + 1] != '.') {    // "..." = no reading
            if (c == 'g') g = v;
            else if (c == 't') { t = v; have_t = true; }
            else if (c == 'r') r = v;
            else if (c == 'p') p24 = v;
            else if (c == 'P') P = v;
            else if (c == 'h') h = v == 0 ? 100 : v;
            else if (c == 'b') b = v;
        }
        i += 1 + n;
    }
    if (have_t) { sep(); o.add(" %.1f °C", (t - 32.0) / 1.8); }
    if (h >= 0) { sep(); o.add(" %s %.0f%%", T("hum.", "hum."), h); }
    if (wind_dir >= 0) {
        sep();
        o.add(" %s %d° %.0f km/h", T("vento", "wind"), wind_dir, wind_mph * 1.609);
        if (g >= 0)
            o.add(" (%s %.0f)", T("raj.", "gust"), g * 1.609);
    }
    if (b >= 0) { sep(); o.add(" %.1f hPa", b / 10.0); }
    if (r >= 0) { sep(); o.add(" %s 1h %.1f mm", T("chuva", "rain"), r * 0.254); }
    if (P >= 0) { sep(); o.add(" %s %.1f mm", T("desde 0h", "since 0h"), P * 0.254); }
    else if (p24 >= 0) { sep(); o.add(" %s 24h %.1f mm", T("chuva", "rain"), p24 * 0.254); }
    return i;
}

// Position report body: position, then weather or comment.
void add_report(Out &o, const char *s, int len)
{
    const Pos p = parse_position(s, len);
    if (!p.ok) {
        o.add("%s: ", T("posição?", "position?"));
        o.raw(s, len);
        return;
    }
    add_position(o, p);
    const char *rest = s + p.used;
    int rlen = len - p.used;
    if (p.code == '_') {    // weather station: "ddd/sss" is the wind
        int dir = -1, spd = 0;
        if (rlen >= 7 && digits(rest, 3) && rest[3] == '/' && digits(rest + 4, 3)) {
            dir = (int)num(rest, 3);
            spd = (int)num(rest + 4, 3);
            rest += 7;
            rlen -= 7;
        }
        const int used = add_weather(o, rest, rlen, dir, spd);
        rest += used;
        rlen -= used;
    }
    add_comment(o, rest, rlen, p.alt_m > -1000);
}

// Mic-E: latitude in the destination call, longitude, speed and course in the
// information field.
void add_mic_e(Out &o, const uint8_t *dest, const char *s, int len)
{
    if (len < 9) {
        o.add("Mic-E: ");
        o.raw(s, len);
        return;
    }
    char d[6];
    for (int i = 0; i < 6; i++)
        d[i] = (char)(dest[i] >> 1);
    int dig[6];
    for (int i = 0; i < 6; i++) {
        const char c = d[i];
        dig[i] = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'J' ? c - 'A' : c >= 'P' && c <= 'Y' ? c - 'P' : 0;
    }
    double lat = dig[0] * 10 + dig[1] + (dig[2] * 10 + dig[3] + (dig[4] * 10 + dig[5]) / 100.0) / 60.0;
    if (!(d[3] >= 'P' && d[3] <= 'Z'))
        lat = -lat;
    const bool offset = d[4] >= 'P' && d[4] <= 'Z';
    const bool west = d[5] >= 'P' && d[5] <= 'Z';
    int deg = (unsigned char)s[1] - 28;
    if (offset)
        deg += 100;
    if (deg >= 180 && deg <= 189)
        deg -= 80;
    else if (deg >= 190 && deg <= 199)
        deg -= 190;
    int min = (unsigned char)s[2] - 28;
    if (min >= 60)
        min -= 60;
    const int hund = (unsigned char)s[3] - 28;
    double lon = deg + (min + hund / 100.0) / 60.0;
    if (west)
        lon = -lon;
    const int sp = (unsigned char)s[4] - 28, dc = (unsigned char)s[5] - 28, se = (unsigned char)s[6] - 28;
    int speed = sp * 10 + dc / 10, course = (dc % 10) * 100 + se;
    if (speed >= 800)
        speed -= 800;
    if (course >= 400)
        course -= 400;
    Pos p;
    p.lat = lat;
    p.lon = lon;
    p.code = s[7];
    p.table = s[8];
    p.course = course;
    p.speed_kmh = speed * 1.852f;
    // Altitude: "xxx}" (base 91, metres above -10 km) in the comment.
    char cbuf[256];
    int clen = len - 9 < (int)sizeof(cbuf) ? len - 9 : (int)sizeof(cbuf);
    memcpy(cbuf, s + 9, clen);
    for (int i = 0; i + 4 <= clen; i++) {
        if (cbuf[i + 3] == '}') {
            p.alt_m = (float)(b91(cbuf + i, 3) - 10000);
            memmove(cbuf + i, cbuf + i + 4, clen - i - 4);    // drop it from the comment
            clen -= 4;
            break;
        }
    }
    const char *c = cbuf;
    o.add("Mic-E ");
    add_position(o, p);
    // Leading radio-type characters ('>', ']', '`', '\'') carry no text.
    while (clen > 0 && strchr(">]`'", *c)) {
        c++;
        clen--;
    }
    add_comment(o, c, clen, true);
}

// ---------------------------------------------------------------------------

void format_aprs(Out &o, const uint8_t *dest, const char *s, int len)
{
    if (len <= 0) {
        o.add("(%s)", T("vazio", "empty"));
        return;
    }
    const char dti = s[0];
    switch (dti) {
    case '!':
    case '=':
        o.add("%s ", T("Posição", "Position"));
        add_report(o, s + 1, len - 1);
        break;
    case '/':
    case '@':    // timestamp DDHHMMz / HHMMSSh / DDHHMM/ first
        o.add("%s ", T("Posição", "Position"));
        if (len >= 8)
            add_report(o, s + 8, len - 8);
        break;
    case '`':
    case '\'':
    case 0x1C:
    case 0x1D:
        add_mic_e(o, dest, s, len);
        break;
    case ':': {    // message ":ADDRESSEE:text{id"
        if (len < 11 || s[10] != ':') {
            o.raw(s, len);
            break;
        }
        char to[10];
        memcpy(to, s + 1, 9);
        to[9] = 0;
        for (int i = 8; i >= 0 && to[i] == ' '; i--)
            to[i] = 0;
        const char *t = s + 11;
        int tlen = len - 11;
        if (!strncmp(to, "BLN", 3)) {
            o.add("%s %s: ", T("Boletim", "Bulletin"), to + 3);
            o.raw(t, tlen);
        } else if (tlen >= 3 && (!strncmp(t, "ack", 3) || !strncmp(t, "rej", 3))) {
            o.add("%s %s %s: ", !strncmp(t, "ack", 3) ? T("Confirmação", "Ack") : T("Recusa", "Reject"),
                  T("para", "to"), to);
            o.raw(t + 3, tlen - 3);
        } else {
            const char *id = (const char *)memchr(t, '{', tlen);
            o.add("%s %s %s: ", T("Mensagem", "Message"), T("para", "to"), to);
            o.raw(t, id ? (int)(id - t) : tlen);
            if (id) {
                o.add(" (#");
                o.raw(id + 1, tlen - (int)(id + 1 - t));
                o.add(")");
            }
        }
        break;
    }
    case '>':
        o.add("%s: ", T("Estado", "Status"));
        if (len >= 8 && digits(s + 1, 6) && s[7] == 'z')
            o.raw(s + 8, len - 8);
        else
            o.raw(s + 1, len - 1);
        break;
    case ';':    // object: name(9) '*'/'_' timestamp(7) position
        if (len >= 18) {
            o.add("%s ", T("Objeto", "Object"));
            o.raw(s + 1, 9);
            if (s[10] == '_')
                o.add(" (%s)", T("removido", "killed"));
            o.add(": ");
            add_report(o, s + 18, len - 18);
        } else {
            o.raw(s, len);
        }
        break;
    case ')': {    // item: name(3-9) '!'/'_' position
        int e = 1;
        while (e < len && e < 11 && s[e] != '!' && s[e] != '_')
            e++;
        o.add("%s ", T("Item", "Item"));
        o.raw(s + 1, e - 1);
        if (e < len && s[e] == '_')
            o.add(" (%s)", T("removido", "killed"));
        o.add(": ");
        if (e < len)
            add_report(o, s + e + 1, len - e - 1);
        break;
    }
    case '_': {    // weather without position: "_MMDDHHMM" then cXXXsXXX...
        o.add("%s", T("Meteo", "Weather"));
        const char *w = s + 9;
        int wl = len - 9;
        int dir = -1, spd = 0;
        if (wl >= 8 && w[0] == 'c' && w[4] == 's') {
            dir = (int)num(w + 1, 3);
            spd = (int)num(w + 5, 3);
            w += 8;
            wl -= 8;
        }
        const int used = wl > 0 ? add_weather(o, w, wl, dir, spd) : 0;
        add_comment(o, w + used, wl - used, true);
        break;
    }
    case 'T':
        o.add("%s: ", T("Telemetria", "Telemetry"));
        o.raw(s + 1, len - 1);
        break;
    case '<':
        o.add("%s: ", T("Capacidades", "Capabilities"));
        o.raw(s + 1, len - 1);
        break;
    case '}':
        o.add("%s: ", T("Terceiros", "Third party"));
        o.raw(s + 1, len - 1);
        break;
    default:
        o.raw(s, len);
        break;
    }
}

}    // namespace

int aprs_format(const uint8_t *frame, int len, bool en, const char *stamp, char *out, size_t size, char *src,
                size_t src_size)
{
    en_lang = en;
    Out o{ out, size };
    out[0] = 0;

    int naddr = 0;
    while (naddr < 10 && (naddr + 1) * 7 <= len) {
        naddr++;
        if (frame[naddr * 7 - 1] & 1)
            break;
    }
    if (naddr < 2 || !(frame[naddr * 7 - 1] & 1) || naddr * 7 + 1 > len)
        return 0;

    char call[12], dest[12];
    format_call(frame + 7, call);
    format_call(frame, dest);
    if (src)
        snprintf(src, src_size, "%s", call);
    const int a_end = naddr * 7;
    const uint8_t ctrl = frame[a_end];
    const bool ui = (ctrl & ~0x10) == 0x03;
    const bool has_pid = (ctrl & 1) == 0 || ui;
    const int pid = has_pid && a_end + 1 < len ? frame[a_end + 1] : -1;
    const int info = a_end + (has_pid ? 2 : 1);
    const bool aprs = ui && pid == 0xF0;

    // Header: time, source, destination, path.
    o.add("%s%s>%s", stamp ? stamp : "", call, dest);
    for (int a = 2; a < naddr; a++) {
        format_call(frame + a * 7, call);
        o.add("%s%s%s", a == 2 ? " via " : ",", call, (frame[a * 7 + 6] & 0x80) ? "*" : "");
    }

    if (aprs) {
        o.add("\n  ");
        format_aprs(o, frame, (const char *)frame + info, len - info);
        o.add("\n");
        return o.n;
    }

    // Any other packet: frame type and everything else, raw.
    if ((ctrl & 1) == 0) {
        o.add(" [I N(S)=%d N(R)=%d%s]", (ctrl >> 1) & 7, (ctrl >> 5) & 7, (ctrl & 0x10) ? " P" : "");
    } else if ((ctrl & 3) == 1) {
        static const char *const S[] = { "RR", "RNR", "REJ", "SREJ" };
        o.add(" [%s N(R)=%d%s]", S[(ctrl >> 2) & 3], (ctrl >> 5) & 7, (ctrl & 0x10) ? " P/F" : "");
    } else {
        const uint8_t m = ctrl & ~0x10;
        const char *name = m == 0x2F ? "SABM" : m == 0x6F ? "SABME" : m == 0x43 ? "DISC" : m == 0x0F ? "DM" :
                           m == 0x63 ? "UA" : m == 0x87 ? "FRMR" : m == 0x03 ? "UI" : m == 0xAF ? "XID" :
                           m == 0xE3 ? "TEST" : "U?";
        o.add(" [%s%s]", name, (ctrl & 0x10) ? " P/F" : "");
    }
    if (pid >= 0)
        o.add(" PID %02X", pid);
    if (info < len) {
        o.add("\n  ");
        o.raw((const char *)frame + info, len - info);
    }
    o.add("\n");
    return o.n;
}
