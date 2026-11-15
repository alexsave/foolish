#!/bin/bash
# run_render.sh [HAE_SEND [extra clang flags]] : compiles and renders the demo into out/ (the send level is the only knob)
cd "$(dirname "$0")"
S=${1:-0.5}; shift
nice clang -O2 -w -DHAE_SEND=$S "$@" -o score score.c && SAMPLES=/Users/alex/Dev/soundtrack-samples ENGINE=../engine nice ./score full out 2>&1 | tail -3
