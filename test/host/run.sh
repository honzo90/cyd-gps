#!/bin/sh
# Host tests for the hardware-independent code. Usage: sh test/host/run.sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p .pio/host_test
CC="cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I lib/odo_core/src -I lib/gpx_core/src -I lib/accel_core/src -I lib/nav_core/src -I lib/cyd_common/src"
$CC lib/odo_core/src/odo_core.c test/host/test_odo.c -lm -o .pio/host_test/test_odo
$CC lib/odo_core/src/odo_core.c lib/gpx_core/src/gpx_core.c test/host/test_gpx.c -lm -o .pio/host_test/test_gpx
$CC lib/accel_core/src/accel_core.c test/host/test_accel.c -lm -o .pio/host_test/test_accel
$CC lib/nav_core/src/nav_core.c test/host/test_nav.c -lm -o .pio/host_test/test_nav
$CC lib/cyd_common/src/cyd_rot.c test/host/test_rot.c -lm -o .pio/host_test/test_rot
.pio/host_test/test_odo
.pio/host_test/test_gpx
.pio/host_test/test_accel
.pio/host_test/test_nav
.pio/host_test/test_rot
node test/host/test_rides.mjs
