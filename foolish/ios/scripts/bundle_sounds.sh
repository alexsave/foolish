#!/bin/bash
# bundle_sounds.sh - put the game's sounds into FoolishKit.framework.
#
# The sounds are generated, never committed (docs/AUDIO.md): soundgen renders
# every take and ffmpeg encodes it into foolish/public/audio/gen/*.m4a, the
# same files the web serves. This script is FoolishKit's build phase: it runs
# `make bundle` (which renders only when soundgen changed since the last time,
# so a build with nothing new costs a stat) and copies the files into the
# framework's resources as sounds/<name>.m4a, where CuePlayer.swift finds them.
#
# THE TAKE TABLE THE APP PLAYS THEM BY is the kernel's (c/build/gen/cue_takes.c,
# soundgen's `ctable`, built into Foolish.xcframework), from the same soundgen
# sources. A file the table names and the bundle lacks is a build failure here,
# never a silent cue on a phone.
#
# A CLEAN ENVIRONMENT, for xcode_kernel.sh's reasons: Xcode's SDKROOT, CC and
# CFLAGS must not reach a host tool's build. ffmpeg comes from Homebrew.
#
#   bundle_sounds.sh <foolish dir> <destination dir>
set -euo pipefail

root=${1:?usage: bundle_sounds.sh <foolish dir> <destination dir>}
dest=${2:?usage: bundle_sounds.sh <foolish dir> <destination dir>}
path="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
clean() { env -i HOME="$HOME" PATH="$path" ${DEVELOPER_DIR:+DEVELOPER_DIR="$DEVELOPER_DIR"} "$@"; }

gen="$root/public/audio/gen"
stamp="$root/tools/soundgen/build/sounds.stamp"
# `npm run sounds` or a clean may have emptied the directory under a stamp that
# still says it is current.
if [ ! -d "$gen" ] || [ -z "$(find "$gen" -name '*.m4a' -print -quit)" ] \
   || [ ! -f "$root/tools/soundgen/build/wav/takes.tsv" ]; then rm -f "$stamp"; fi
clean make -s -C "$root/tools/soundgen" bundle

# Every file the table names, and nothing it does not: the manifest `sounds`
# leaves beside its WAVs (one line a take, its name first) is that list.
names=$(awk '{ print $1 ".m4a" }' "$root/tools/soundgen/build/wav/takes.tsv")
mkdir -p "$dest"
find "$dest" -name '*.m4a' -type f | while read -r f; do
    grep -qx "$(basename "$f")" <<< "$names" || rm -f "$f"
done
n=0
for f in $names; do
    [ -f "$gen/$f" ] || { echo "error: bundle_sounds: $gen/$f is missing (run npm run sounds in foolish/)" >&2; exit 1; }
    cmp -s "$gen/$f" "$dest/$f" || cp "$gen/$f" "$dest/$f"
    n=$((n + 1))
done
echo "bundle_sounds: $n files, $(du -sk "$dest" | cut -f1) KB in $dest"
