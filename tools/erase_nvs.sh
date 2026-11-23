#!/bin/sh
# Erase only the NVS partition (touch calibration, odometer) so the next boot
# starts calibration. BOOT+RST does NOT work: it enters the serial bootloader.
set -eu
PORT="${1:-$(ls /dev/cu.usbserial-* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no /dev/cu.usbserial-* port found"; exit 1; }
PY="$HOME/.platformio/penv/bin/python"
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
echo "erasing NVS on $PORT"
"$PY" "$ESPTOOL" --port "$PORT" erase_region 0x9000 0x5000
