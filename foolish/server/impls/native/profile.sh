#!/usr/bin/env bash
# profile.sh — portable hot-instruction profiler for the native server /
# standalone kernel harnesses. Picks the best tool the HOST actually has:
#
#   macOS, `sample` present   -> attach to a running PID with `sample`
#                                 (real multi-core wall-clock sampling —
#                                 this is the path a developer on a Mac runs
#                                 against a live, multi-threaded server).
#   Linux, `perf` present     -> `perf record -g` (attach OR launch) +
#                                 `perf report --stdio`.
#   else (valgrind present)   -> `valgrind --tool=callgrind` to LAUNCH the
#                                 target (callgrind instruments from process
#                                 start; it cannot attach to something
#                                 already running), then `callgrind_annotate`
#                                 for hot functions + hot source lines.
#
# This box has neither `sample` nor `perf` — only valgrind/callgrind and
# gprof — so here every profile is `--launch`, and it single-threads
# whatever it instruments (callgrind serializes; see PROFILE_HOTPATH.md's
# note on the server's real concurrency picture).
#
# Usage:
#   profile.sh --launch <label> -- <cmd...>       # callgrind / perf record launches <cmd>
#   profile.sh --attach <label> <pid> <secs>      # sample / perf record attaches to <pid>
#
# Output: server/impls/native/bench_results/<label>/
#   raw/            the profiler's native dump (gitignored — can be large)
#   annotated.txt   the digested, human-readable hot-function/hot-line report
#                    (small; this is what gets committed)
#   meta.txt        which tool ran, the exact command, start time
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_ROOT="$SCRIPT_DIR/bench_results"

usage() {
    cat >&2 <<'EOF'
usage:
  profile.sh --launch <label> -- <cmd...>
  profile.sh --attach <label> <pid> <secs>
EOF
    exit 2
}

[ $# -ge 1 ] || usage
MODE="$1"; shift
case "$MODE" in
    --launch)
        [ $# -ge 1 ] || usage
        LABEL="$1"; shift
        if [ "${1:-}" = "--" ]; then shift; fi
        [ $# -ge 1 ] || { echo "profile.sh --launch: need a command after --" >&2; usage; }
        CMD=("$@")
        ;;
    --attach)
        [ $# -eq 3 ] || usage
        LABEL="$1"; PID="$2"; SECS="$3"
        ;;
    -h|--help)
        usage
        ;;
    *)
        usage
        ;;
esac

OUT_DIR="$OUT_ROOT/$LABEL"
RAW_DIR="$OUT_DIR/raw"
mkdir -p "$RAW_DIR"
ANNOTATED="$OUT_DIR/annotated.txt"
META="$OUT_DIR/meta.txt"

OS="$(uname)"
have() { command -v "$1" >/dev/null 2>&1; }

{
    echo "label:      $LABEL"
    echo "mode:       $MODE"
    echo "host uname: $OS $(uname -m)"
    echo "started:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$META"

# ---------------------------------------------------------------------------
# Tool selection
# ---------------------------------------------------------------------------

if [ "$OS" = "Darwin" ] && have sample; then
    TOOL="sample"
elif [ "$OS" != "Darwin" ] && have perf; then
    TOOL="perf"
elif have valgrind; then
    TOOL="callgrind"
else
    echo "profile.sh: no supported profiler found (need macOS 'sample', Linux 'perf', or 'valgrind')" >&2
    exit 1
fi
echo "tool:       $TOOL" >> "$META"

# ---------------------------------------------------------------------------
# sample (macOS, attach only — this is the multi-core path)
# ---------------------------------------------------------------------------

run_sample() {
    if [ "$MODE" != "--attach" ]; then
        echo "profile.sh: 'sample' only attaches to a running process — use --attach <label> <pid> <secs>." >&2
        echo "            (to profile a freshly-launched short-lived tool, start it, note its PID, then attach.)" >&2
        exit 1
    fi
    echo "cmd:        sample $PID $SECS -file $RAW_DIR/sample.txt" >> "$META"
    echo "== sample: attaching to pid $PID for ${SECS}s (real multi-core wall-clock sampling) =="
    sample "$PID" "$SECS" -file "$RAW_DIR/sample.txt"
    cp "$RAW_DIR/sample.txt" "$ANNOTATED"
    echo "== wrote $ANNOTATED =="
}

# ---------------------------------------------------------------------------
# perf (Linux, launch or attach)
# ---------------------------------------------------------------------------

run_perf() {
    DATA="$RAW_DIR/perf.data"
    if [ "$MODE" = "--launch" ]; then
        echo "cmd:        perf record -g -o $DATA -- ${CMD[*]}" >> "$META"
        echo "== perf record -g: launching ${CMD[*]} =="
        perf record -g -o "$DATA" -- "${CMD[@]}"
    else
        echo "cmd:        perf record -g -p $PID -o $DATA -- sleep $SECS" >> "$META"
        echo "== perf record -g: attaching to pid $PID for ${SECS}s =="
        perf record -g -p "$PID" -o "$DATA" -- sleep "$SECS"
    fi
    echo "== perf report --stdio -> $ANNOTATED =="
    perf report --stdio -i "$DATA" > "$ANNOTATED" 2>&1
    echo "== wrote $ANNOTATED (raw: $DATA) =="
}

# ---------------------------------------------------------------------------
# callgrind (valgrind) — launch only. Instruction-level, per-function AND
# per-source-line attribution via callgrind_annotate. Serializes threads
# (single logical CPU under the JIT'd instrumentation), and is ~30-50x
# slower than native execution — keep launched workloads short (see
# PROFILE_HOTPATH.md for the exact bounded runs used here).
# ---------------------------------------------------------------------------

run_callgrind() {
    if [ "$MODE" = "--attach" ]; then
        cat >&2 <<EOF
profile.sh: callgrind cannot attach to an already-running process — it
            instruments from process start (valgrind re-execs the target
            under its JIT). Use --launch <label> -- <cmd...> instead: start
            the SAME binary under 'profile.sh --launch' rather than starting
            it separately and attaching. (On this box valgrind is the only
            available profiler, so every T1/T2/T3 run in PROFILE_HOTPATH.md
            uses --launch.)
EOF
        exit 1
    fi
    OUTFILE="$RAW_DIR/callgrind.out"
    echo "cmd:        valgrind --tool=callgrind --callgrind-out-file=$OUTFILE -- ${CMD[*]}" >> "$META"
    echo "== callgrind: launching ${CMD[*]} (this is ~30-50x slower than native — keep it short) =="
    valgrind --tool=callgrind --callgrind-out-file="$OUTFILE" -- "${CMD[@]}"
    VG_STATUS=$?
    if [ ! -f "$OUTFILE" ]; then
        echo "profile.sh: callgrind produced no output file ($OUTFILE) — target may have failed to start (exit $VG_STATUS)" >&2
        exit 1
    fi
    echo "== callgrind_annotate --auto=yes --inclusive=no -> $ANNOTATED =="
    callgrind_annotate --auto=yes --inclusive=no "$OUTFILE" > "$ANNOTATED" 2>&1
    echo "== wrote $ANNOTATED (raw: $OUTFILE, target exit=$VG_STATUS) =="
}

case "$TOOL" in
    sample)    run_sample ;;
    perf)      run_perf ;;
    callgrind) run_callgrind ;;
esac

echo "finished:   $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$META"
