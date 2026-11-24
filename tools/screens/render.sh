#!/bin/sh
# PNG of every CYD screen (landscape + portrait) rendered on the Mac from src/main.cpp + LVGL.
# Usage: sh tools/screens/render.sh [outdir]   (default: .pio/screens). Needs a prior `pio run`
# (for the LVGL sources in .pio/libdeps) and the Xcode command line tools.
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-.pio/screens}
B=.pio/screens-build
L=.pio/libdeps/cyd2usb/lvgl
mkdir -p "$OUT" "$B/obj"
CF="-O1 -w -DLV_CONF_INCLUDE_SIMPLE -Itools/screens/host -Iinclude -I$L"
if [ ! -f "$B/liblvgl.a" ]; then
  echo "building LVGL for the host (once)..."
  find "$L/src" -name '*.c' | grep -vE '/(drivers|others/vg_lite|draw/(vg_lite|nxp|renesas|sdl|opengles|dave2d|nema_gfx)|libs/(ffmpeg|freetype|libpng|lodepng|tjpgd|libjpeg|gif|bmp|rlottie|thorvg|tiny_ttf|barcode|fsdrv)|osal/(cmsis|freertos|pthread|rtthread|windows|sdl2|mqx))' |
  while read f; do cc -c $CF "$f" -o "$B/obj/$(echo "$f" | tr '/.' '__').o"; done
  ar rcs "$B/liblvgl.a" "$B"/obj/*.o
fi
INC="-Itools/screens/host -Isrc -Ilib/accel_core/src -Ilib/cyd_common/src -Ilib/nav_core/src -Ilib/odo_core/src"
LIBC="lib/accel_core/src/accel_core.c lib/accel_core/src/acc_hist.c lib/nav_core/src/nav_core.c lib/odo_core/src/odo_core.c lib/cyd_common/src/cyd_calib_ui.c lib/cyd_common/src/touch_cal.c lib/cyd_common/src/cyd_rot.c"
for f in $LIBC; do cc -c $CF $INC "$f" -o "$B/$(basename "$f" .c).o"; done
c++ -std=c++17 $CF $INC -DFW_VERSION='"host"' tools/screens/render.cpp src/seg7.cpp "$B"/accel_core.o "$B"/acc_hist.o \
  "$B"/nav_core.o "$B"/odo_core.o "$B"/cyd_calib_ui.o "$B"/touch_cal.o "$B"/cyd_rot.o "$B/liblvgl.a" -o "$B/render"
"$B/render" "$OUT"
for p in "$OUT"/*.ppm; do sips -s format png "$p" --out "${p%.ppm}.png" >/dev/null && rm "$p"; done
ls "$OUT"
