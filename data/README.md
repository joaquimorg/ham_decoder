# data/

`jsc_dict.bin` is the word dictionary JS8 uses to compress text (JSC, 262144
words), taken from JS8Call's `jsc_map.cpp` by `tools/js8/gen_tables.py`, which
also writes `src/js8_ldpc.inc` (the JS8 LDPC code) from JS8Call's
`lib/ft8/bpdecode174.f90` and `ldpc_174_87_params.f90`. The firmware embeds the
dictionary (`board_build.embed_files` in `platformio.ini`) to show compressed
JS8 text; without it those frames show as `<JSC>`.

JS8Call (https://github.com/js8call/js8call) is GPLv3, while this project is
MIT. The dictionary and the code tables are what the JS8 protocol is defined
by, but if that matters for how you use or distribute the firmware, check the
licence: the dictionary can be left out (delete the file and the two lines that
embed it) at the cost of JS8 text frames.

Format: a little-endian uint32 offset into the word area every 64 words, then
the words, NUL-terminated, in index order.
