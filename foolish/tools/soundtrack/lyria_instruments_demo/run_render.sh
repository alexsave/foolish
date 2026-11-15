#!/bin/bash
# run_render.sh [HAE_SEND] : compiles and renders the demo into out/ (the send level is the only knob)
cd "$(dirname "$0")"
nice clang -O2 -w -DHAE_SEND=${1:-0.3} -o score score.c && SAMPLES=/Users/alex/Dev/soundtrack-samples ENGINE=../engine nice ./score full out 2>&1 | tail -3
