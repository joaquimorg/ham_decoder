#!/bin/sh
# Regenerates components/ui_fonts/font_ui_{12,14,16}.c: LVGL's Montserrat Medium plus
# Latin-1 (0xA0-0xFF: the Portuguese accented letters, which LVGL's built-in
# fonts lack) and the same FontAwesome symbols as the built-in fonts.
# Needs Node (npx) and the two source fonts from the LVGL repository
# (scripts/built_in_font/ on the release/v9.2 branch) in the current directory.
set -e
SYM=61441,61448,61451,61452,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650
OUT=${1:-components/ui_fonts}
for sz in 12 14 16; do
    npx --yes lv_font_conv --no-compress --no-prefilter --bpp 4 --size $sz \
        --font Montserrat-Medium.ttf -r 0x20-0x7E,0xA0-0xFF,0x2022 \
        --font FontAwesome5-Solid+Brands+Regular.woff -r $SYM \
        --format lvgl --lv-font-name font_ui_$sz --force-fast-kern-format -o "$OUT/font_ui_$sz.c"
    # The component exposes "lvgl.h" only.
    sed -i '/#ifdef LV_LVGL_H_INCLUDE_SIMPLE/,/#endif/c\#include "lvgl.h"' "$OUT/font_ui_$sz.c"
done
