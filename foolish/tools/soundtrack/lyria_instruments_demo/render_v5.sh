#!/bin/bash
# render_v5.sh TAG [flags...]: renders the demo with the given -D flags and keeps out/full_<TAG>.wav (out/full.wav is overwritten each time)
cd "$(dirname "$0")"
T=$1; shift
./run_render.sh 0.5 "$@" | tail -1
cp out/full.wav out/full_$T.wav
