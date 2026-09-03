#!/bin/sh
# Host-side unit tests.
#
# Only the parts that do not need luna-service2/pbnjson/cups are covered here;
# those three only exist in the Yocto sysroot. What is covered is the logic
# most likely to be silently wrong: the webOS<->IPP keyword mapping and the
# printable-area arithmetic, the latter checked against numbers captured from a
# real TouchPad running the original daemon.
set -e
cd "$(dirname "$0")"
mkdir -p build

CFLAGS="-Wall -Wextra -std=gnu99 -I../src"
GLIB_CFLAGS=$(pkg-config --cflags glib-2.0)
GLIB_LIBS=$(pkg-config --libs glib-2.0)

echo "=== param_map ==="
gcc $CFLAGS -Werror \
    test_param_map.c ../src/param_map.c ../src/print_errors.c \
    -o build/test_param_map
./build/test_param_map

echo
echo "=== printable area ==="
gcc $CFLAGS $GLIB_CFLAGS \
    test_area.c ../src/job_manager.c ../src/print_errors.c \
    $GLIB_LIBS -lm -o build/test_area
./build/test_area
