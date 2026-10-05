#!/bin/sh
# Builds the decoder self-tests for the PC and runs them (in WSL or Linux):
#   tools/host_test/run.sh aprs|psk|cw|...
# Each test compiles the firmware's decoder source with the stubs here and
# the decoder's *_SELFTEST switched on.
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-build/host_test}
mkdir -p "$OUT"
CXX=${CXX:-g++}
FLAGS="-O2 -std=gnu++17 -Itools/host_test/stubs -Iinclude -Wall -Wno-unused-function"
case "$1" in
aprs) $CXX $FLAGS -DAPRS_SELFTEST=1 src/aprs_decoder.cpp src/aprs_format.cpp tools/host_test/stubs/settings_stub.cpp \
          tools/host_test/main_aprs.cpp -o "$OUT/aprs" && "$OUT/aprs" ;;
psk) $CXX $FLAGS -DPSK_SELFTEST=1 src/psk_decoder.cpp tools/host_test/main_psk.cpp -o "$OUT/psk" && "$OUT/psk" ;;
cw) $CXX $FLAGS src/cw_decoder.cpp tools/host_test/main_cw.cpp -o "$OUT/cw" && "$OUT/cw" ;;
qrss) $CXX $FLAGS src/qrss.cpp src/fft.cpp tools/host_test/main_qrss.cpp -o "$OUT/qrss" && "$OUT/qrss" ;;
skim) $CXX $FLAGS src/skimmer.cpp src/cw_decoder.cpp src/psk_decoder.cpp tools/host_test/main_skim.cpp -o "$OUT/skim" && "$OUT/skim" ;;
mfsk) $CXX $FLAGS src/mfsk_decoder.cpp src/fft.cpp tools/host_test/main_mfsk.cpp -o "$OUT/mfsk" && "$OUT/mfsk" ;;
js8)
    LIB=components/ft8_lib
    $CXX $FLAGS -I$LIB -x c $LIB/common/monitor.c $LIB/fft/kiss_fft.c $LIB/fft/kiss_fftr.c -x c++ \
        src/js8_decoder.cpp tools/host_test/main_js8.cpp -o "$OUT/js8" && "$OUT/js8" ;;
*) echo "uso: $0 aprs|psk|cw|qrss|skim|mfsk|js8"; exit 1 ;;
esac
