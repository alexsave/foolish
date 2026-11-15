#!/bin/bash
# stems.sh - separate one audio file into drums, bass, other and vocals stems with demucs (htdemucs, CPU).
#
# Inputs:   $1 = audio file (mp3 or wav), $2 = output directory (created if missing).
# Outputs:  $2/htdemucs/<file name without extension>/{drums,bass,other,vocals}.wav (44.1 kHz stereo).
# Example:  tools/transcribe/stems.sh /path/clip.mp3 /path/stems
# Tool locations (overridable environment variables, no other path is hard-coded):
#   SOUNDTRACK_VENV  python venv holding demucs and basic-pitch (default /Users/alex/Dev/soundtrack-tools/venv)
#   TORCH_HOME       demucs model weights, kept outside the repo (default /Users/alex/Dev/soundtrack-tools/torch_home)
# How the venv was made: `uv venv --python 3.11` then
#   `uv pip install basic-pitch demucs onnxruntime 'setuptools<81' --exclude-newer 2026-10-31`.
# The weights are fetched once into TORCH_HOME by demucs on its first run; this script never installs or downloads
# anything itself, and a missing weight file makes demucs try the network and fail offline.
# Runs under `nice -n 10`; the caller is responsible for running only one heavy job at a time.
# Known failure modes: about 1 minute of CPU per 2 minutes of audio; a missing venv or weights gives a non-zero exit;
#   very short files (under about 1 s) can fail inside demucs; stems of mono or near-silent input are near-silent;
#   the separator leaks: the "vocals" stem holds whatever the model decided is voice-like (leads, choirs, strings).
set -eu
if [ "$#" -ne 2 ]; then
  echo "usage: $0 FILE OUTDIR" >&2
  exit 2
fi
VENV="${SOUNDTRACK_VENV:-/Users/alex/Dev/soundtrack-tools/venv}"
export TORCH_HOME="${TORCH_HOME:-/Users/alex/Dev/soundtrack-tools/torch_home}"
mkdir -p "$2"
exec nice -n 10 "$VENV/bin/python" -m demucs.separate -n htdemucs -d cpu -o "$2" "$1"
