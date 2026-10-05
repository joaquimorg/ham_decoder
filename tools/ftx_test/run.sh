#!/bin/sh
# Builds and runs ftx_test on the PC with any C/C++ compiler (gcc, clang or
# "python -m ziglang cc"): CC="python -m ziglang cc" CXX="python -m ziglang c++" tools/ftx_test/run.sh
set -e
cd "$(dirname "$0")/../.."
CC=${CC:-cc}
CXX=${CXX:-c++}
OUT=${OUT:-build/ftx_test}
mkdir -p "$OUT"
LIB=components/ft8_lib
# stpcpy is missing on Windows C libraries.
printf '#include <string.h>\n#ifdef _WIN32\nstatic inline char *stpcpy(char *d, const char *s) { size_t n = strlen(s); memcpy(d, s, n + 1); return d + n; }\n#endif\n' > "$OUT/shim.h"
OBJS=""
for f in $LIB/ft8/constants.c $LIB/ft8/crc.c $LIB/ft8/decode.c $LIB/ft8/encode.c $LIB/ft8/ldpc.c \
         $LIB/ft8/message.c $LIB/ft8/text.c $LIB/common/monitor.c $LIB/fft/kiss_fft.c $LIB/fft/kiss_fftr.c; do
    o="$OUT/$(basename "$f" .c).o"
    $CC -O2 -w -I$LIB -include "$OUT/shim.h" -c "$f" -o "$o"
    OBJS="$OBJS $o"
done
$CXX -O2 -std=c++17 -Iinclude -I$LIB src/ftx_core.cpp src/js8_decoder.cpp tools/ftx_test/ftx_test.cpp $OBJS -o "$OUT/ftx_test"
"$OUT/ftx_test"
