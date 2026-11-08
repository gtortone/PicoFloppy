#!/bin/sh
# mk_dist.sh [version]
#
# Build the three firmware levels (prod, debug, logfile) with CMake and
# package them as a release zip: dist/PicoFloppy-<version>.zip. The version
# defaults to the short git hash, and is also the firmware version shown at
# boot. Used by the CI and release workflows (.github/workflows).
#
# Needs PICO_SDK_PATH, the usual build tools (see README.md), wget, zip
# and unzip. Extra CMake options can be passed in CMAKE_FLAGS.
#
# This is free and unencumbered software released into the public domain.
# See the file COPYING for more details, or visit <http://unlicense.org>.

set -e

cd "$(dirname "$0")/.."

PROJ=PicoFloppy
VER=${1:-$(git rev-parse --short HEAD)}
JOBS=$(nproc 2>/dev/null || echo 4)

HXC_VER=v9-FF
HXC_ZIP=HxC_Compat_Mode-$HXC_VER.zip
HXC_URL=https://github.com/keirf/flashfloppy-hxc-file-selector/releases/download/$HXC_VER/$HXC_ZIP

for level in prod debug logfile; do
    # shellcheck disable=SC2086 # CMAKE_FLAGS is a list of options
    cmake -S . -B build/$level -DFF_LEVEL=$level -DFF_FW_VER="$VER" \
          $CMAKE_FLAGS
    cmake --build build/$level -j "$JOBS"
done

OUT=dist/$PROJ-$VER
rm -rf "$OUT" "$OUT.zip"
mkdir -p "$OUT/alt/debug" "$OUT/alt/logfile" "$OUT/scripts"

cp build/prod/picofloppy.uf2 "$OUT/$PROJ-$VER.uf2"
cp build/debug/picofloppy.uf2 "$OUT/alt/debug/$PROJ-debug-$VER.uf2"
cp build/logfile/picofloppy.uf2 "$OUT/alt/logfile/$PROJ-logfile-$VER.uf2"
cp -a LICENSE README.md examples "$OUT/"
cp -a scripts/edsk*.py scripts/mk_hfe.py scripts/mk_fat.py \
      scripts/mk_uf2.py "$OUT/scripts/"

# HxC compatibility-mode files (FlashFloppy's HxC file selector).
mkdir -p dist/ext
if [ ! -e "dist/ext/$HXC_ZIP" ]; then
    wget -q -O "dist/ext/$HXC_ZIP.part" "$HXC_URL"
    mv "dist/ext/$HXC_ZIP.part" "dist/ext/$HXC_ZIP"
fi
(cd "$OUT" && unzip -q "../ext/$HXC_ZIP")

# Licenses of the third-party code linked into the firmware.
mkdir -p "$OUT/licenses"
U8G2_DIR=$(sed -n 's/^U8G2_SOURCE_DIR:PATH=//p' build/prod/CMakeCache.txt)
cp "${U8G2_DIR:-build/prod/_deps/u8g2}/LICENSE" "$OUT/licenses/u8g2.txt"
cp "$PICO_SDK_PATH/LICENSE.TXT" "$OUT/licenses/pico-sdk.txt"
cp "$PICO_SDK_PATH/lib/tinyusb/LICENSE" "$OUT/licenses/tinyusb.txt"
sed -n '1,/\*\//p' src/fatfs/ff.c > "$OUT/licenses/fatfs.txt"

git rev-parse HEAD > "$OUT/COMMIT"

(cd dist && zip -qr "$PROJ-$VER.zip" "$PROJ-$VER")
echo "dist/$PROJ-$VER.zip"
