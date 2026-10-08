#pragma once

#include <ctype.h>
#include <string.h>

// Maidenhead locator (4 or 6 characters) at the end of an FT8/FT4/JS8 message
// ("CQ CT1ABC IM58", "CT1ABC EA1XYZ JN11", "CT1ABC: @HB HEARTBEAT IM58"):
// the centre of its square, for the station map. "RR73" is a closing, not a
// locator.
inline bool maidenhead_from_text(const char *text, float *lat, float *lon)
{
    const char *end = text + strlen(text);
    while (end > text && end[-1] == ' ')
        end--;
    const char *p = end;
    while (p > text && p[-1] != ' ')
        p--;
    const int n = (int)(end - p);
    if (n != 4 && n != 6)
        return false;
    if (n == 4 && strncmp(p, "RR73", 4) == 0)
        return false;
    const int f0 = toupper((unsigned char)p[0]) - 'A', f1 = toupper((unsigned char)p[1]) - 'A';
    if (f0 < 0 || f0 > 17 || f1 < 0 || f1 > 17 || !isdigit((unsigned char)p[2]) || !isdigit((unsigned char)p[3]))
        return false;
    float lo = -180.0f + f0 * 20.0f + (p[2] - '0') * 2.0f, la = -90.0f + f1 * 10.0f + (p[3] - '0');
    float dlo = 2.0f, dla = 1.0f;
    if (n == 6) {
        const int s0 = toupper((unsigned char)p[4]) - 'A', s1 = toupper((unsigned char)p[5]) - 'A';
        if (s0 < 0 || s0 > 23 || s1 < 0 || s1 > 23)
            return false;
        lo += s0 * (2.0f / 24.0f);
        la += s1 * (1.0f / 24.0f);
        dlo = 2.0f / 24.0f;
        dla = 1.0f / 24.0f;
    }
    *lon = lo + dlo / 2;
    *lat = la + dla / 2;
    return true;
}
