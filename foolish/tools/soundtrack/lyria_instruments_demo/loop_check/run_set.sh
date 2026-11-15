#!/bin/bash
# run_set.sh PREFIX ENGINE_ORCH_DIR LOOPMODE HAE_SEND [extra clang flags]
# Renders the 5 s held notes (PREFIX = before or after) and prints the analysis table (tab separated) to stdout.
P="$1"; E="$2"; LM="$3"; HS="$4"; shift 4
W=/private/tmp/claude-501/-Users-alex-Dev-amgi/8dce4517-583f-417d-bdf2-8f13b7e41f4b/scratchpad/lyria/lc
PNG=/Users/alex/Music/foolish_cities_v5/replicas/instruments/loop_check; mkdir -p $PNG
H=$(dirname "$0")
one(){ # inst note vel rel send tag
  d=$W/${P}_$1_$2_$6; "$H/render_one.sh" "$E" "$d" $1 $LM $5 $4 $2 $3 "${@:7}" >/dev/null 2>&1
  python3 -I "$H/analyse_loop.py" "$d" "${P}_$1_$2_$6" "$PNG/${P}_$1_$2_$6.png"
}
one cello D2 0.8 0.634 0 s0 "$@"; one cello G3 0.8 0.634 0 s0 "$@"; one cello D4 0.8 0.634 0 s0 "$@"
one haegeum C4 0.8 0.0716 $HS send "$@"; one haegeum C5 0.8 0.0716 $HS send "$@"; one haegeum G5 0.8 0.0716 $HS send "$@"
one haegeum C4 0.8 0.0716 0 s0 "$@"; one haegeum C5 0.8 0.0716 0 s0 "$@"; one haegeum G5 0.8 0.0716 0 s0 "$@"
one trumpet D4 0.8 0.319 0 L1 "$@"; one trumpet A#4 0.8 0.319 0 L1 "$@"; one trumpet A#4 0.3 0.319 0 L0 "$@"; one trumpet F5 0.8 0.319 0 L1 "$@"
