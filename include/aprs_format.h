#pragma once

#include <stddef.h>
#include <stdint.h>

// Text for one valid AX.25 frame (without the FCS), as the APRS decoder shows it:
//  - APRS (UI frame, PID F0): a header line (source, path) and an indented
//    line with the interpreted information - position in degrees with the
//    symbol named, course/speed, altitude, weather in metric units, messages,
//    status, objects/items, Mic-E;
//  - any other packet: everything, raw (addresses, frame type, PID, data).
// Lines end in '\n'; the second line of an APRS frame starts with two spaces.
// en = English texts, else Portuguese. Returns the length written.
// `src` gets the source call ("CALL-SSID").
int aprs_format(const uint8_t *frame, int len, bool en, const char *stamp, char *out, size_t size,
                char *src, size_t src_size);
