#!/bin/bash
# notes.sh - transcribe one audio file to notes with basic-pitch (CSV note events plus MIDI).
#
# Inputs:   $1 = audio file (wav or mp3), $2 = output directory (created if missing),
#           $3 = optional label; the outputs are named <label>_basic_pitch.* instead of <file name>_basic_pitch.*
#           (needed when many stems are all called bass.wav, other.wav and so on).
# Outputs:  $2/<label or file name>_basic_pitch.csv  columns start_time_s,end_time_s,pitch_midi,velocity,pitch_bend
#           (the tool appends extra trailing pitch-bend columns; readers must parse only the first five),
#           and $2/<label or file name>_basic_pitch.mid.
# Example:  tools/transcribe/notes.sh /path/stems/htdemucs/clip/bass.wav /path/transcribe/city clip_bass
# Tool location (overridable environment variable): SOUNDTRACK_VENV (default /Users/alex/Dev/soundtrack-tools/venv).
# How the venv was made: `uv venv --python 3.11` then
#   `uv pip install basic-pitch demucs onnxruntime 'setuptools<81' --exclude-newer 2026-10-31`.
# Runs under `nice -n 10`; the caller is responsible for running only one heavy job at a time.
# Known failure modes: polyphonic or noisy input gives ghost notes and short fragments (many under 60 ms);
#   the pitch range is MIDI 21-108 only; drum-heavy input produces spurious notes; basic-pitch trims the first
#   fraction of a second differently from the audio clock by a few ms; velocity is a model output, not a level.
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "usage: $0 FILE OUTDIR [LABEL]" >&2
  exit 2
fi
VENV="${SOUNDTRACK_VENV:-/Users/alex/Dev/soundtrack-tools/venv}"
IN="$1"
OUT="$2"
mkdir -p "$OUT"
if [ "$#" -eq 3 ]; then
  EXT="${IN##*.}"
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  ln -s "$(cd "$(dirname "$IN")" && pwd)/$(basename "$IN")" "$TMP/$3.$EXT"
  IN="$TMP/$3.$EXT"
fi
nice -n 10 "$VENV/bin/basic-pitch" "$OUT" "$IN" --save-midi --save-note-events
