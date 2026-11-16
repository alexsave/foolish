#!/bin/bash
# one-factor experiments on the expert release: render single_notes.txt with the options after the name, then print the release window numbers (charts/release_study.py)
#   charts/release_exp.sh NAME [renderer options...]
cd "$(dirname "$0")/.." || exit 1
name=$1; shift
nice ./altcello render /Users/alex/Music/foolish_cities_v5/alt_cello/scores/single_notes.txt out/wedge/rel_$name.wav --room off --trace out/wedge/rel_$name "$@" > /dev/null || exit 1
echo "== $name $*"
/Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/release_study.py out/wedge/rel_$name.wav out/wedge/rel_$name 2.0,5.0,8.0 43,50,57
