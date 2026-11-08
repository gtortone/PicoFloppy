#!/bin/sh
# update_u8g2.sh <u8g2-checkout>
#
# Refresh the vendored subset of u8g2 (https://github.com/olikraus/u8g2)
# in lib/u8g2/ from a checkout of the u8g2 repository. Only the graphics
# core and the display drivers FlashFloppy uses are copied: the complete
# repository is about 400MB, mostly fonts, and the firmware brings its own
# fonts (src/display/oled_fonts.c, see mk_u8g2_fonts.sh).
#
# To support another display controller, add its u8x8_d_*.c file below.
#
# This is free and unencumbered software released into the public domain.
# See the file COPYING for more details, or visit <http://unlicense.org>.

set -e

if [ $# -ne 1 ] || [ ! -f "$1/csrc/u8g2.h" ]; then
    echo "usage: $0 <u8g2-checkout>" >&2
    exit 1
fi

SRC="$1/csrc"
DST="$(dirname "$0")/../lib/u8g2"

FILES="
u8g2.h u8x8.h
u8g2_arc.c u8g2_bitmap.c u8g2_box.c u8g2_buffer.c u8g2_circle.c
u8g2_cleardisplay.c u8g2_font.c u8g2_hvline.c u8g2_intersection.c
u8g2_kerning.c u8g2_line.c u8g2_ll_hvline.c u8g2_polygon.c u8g2_setup.c
u8x8_8x8.c u8x8_byte.c u8x8_cad.c u8x8_capture.c u8x8_display.c u8x8_gpio.c
u8x8_setup.c
u8x8_string.c u8x8_u16toa.c u8x8_u8toa.c
u8x8_d_ssd1306_128x64_noname.c
u8x8_d_ssd1306_128x32.c
"

rm -rf "$DST/csrc"
mkdir -p "$DST/csrc"
for f in $FILES; do
    cp "$SRC/$f" "$DST/csrc/"
done
cp "$1/LICENSE" "$DST/LICENSE"
VER=$(git -C "$1" describe --tags 2>/dev/null || echo unknown)
echo "$VER" > "$DST/VERSION"
echo "u8g2 $VER copied to $DST"
