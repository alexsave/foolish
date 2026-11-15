#!/bin/bash
# Builds and runs the geometry tool and its test (light CPU: nice, serial).
set -e
cd "$(dirname "$0")"
CFLAGS="-O2 -Wall -Wextra -std=c11"
nice -n 15 clang $CFLAGS -o geomdump geomdump.c geom.c -lm
nice -n 15 clang $CFLAGS -o test_geom test_geom.c geom.c -lm
if [ "$1" = "test" ]; then ./test_geom; fi
