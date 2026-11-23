#!/bin/sh
# Host tests for the hardware-independent code. Usage: sh test/host/run.sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p .pio/host_test
CC="cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I lib/odo_core/src -I lib/gpx_core/src"
$CC lib/odo_core/src/odo_core.c test/host/test_odo.c -lm -o .pio/host_test/test_odo
$CC lib/odo_core/src/odo_core.c lib/gpx_core/src/gpx_core.c test/host/test_gpx.c -lm -o .pio/host_test/test_gpx
.pio/host_test/test_odo
.pio/host_test/test_gpx
