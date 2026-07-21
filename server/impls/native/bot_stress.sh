#!/usr/bin/env bash
# bot_stress.sh — Stage 4's baseline measurement (SERVER_SCALING.md "Stage 4
# — spectators + octogen stress"): the heavy octogen-bot + human-client +
# spectator workload, run at a few scales, with REAL numbers for the things
# Stage 5's kernel-side bot_drive fix will be judged against.
#
# Two separate sweeps, both against the REAL foolish_server + foolish_hammer
# (no synthetic bypass of the server):
#
#   A) FULL-STRESS: the max-stress mix as specified — 1 server-side octogen
#      bot + --seats=7 human WS clients + spectators, per game — at a few
#      game counts. This is where applied human-moves/sec, round-trip
#      latency, RSS, and "is the box CPU-saturated" come from.
#
#   B) SCALING: the SAME per-game shape (1 octogen bot + N human clients),
#      but --seats=1 (the minimum) so far MORE concurrent games fit inside
#      this server's MAX_GAMES=256/MAX_USERS=512 caps — this is what lets
#      the sweep actually REACH the point where aggregate octogen-decision
#      DEMAND exceeds g_kernel_lock's serialized supply. Why a second sweep
#      is needed at all (this is the single most important methodology note
#      in this script — see the README-style comment further down and
#      SERVER_SCALING.md's "Stage 4" section for the full writeup): the
#      KERNEL's own bot_cycle_delay_ms paces every VISIBLE bot move at
#      3000ms whenever a human seat is present (bot_drive.h/bot_drive.c,
#      read-only to this stage) — a UX decision (let a human watching the
#      board keep up), not a capacity limit. That floors EVERY game's own
#      octogen decision rate at ~1/3.05s =~ 0.33/s REGARDLESS of contention,
#      so at the small "1 game or 8" scale the task names as an example,
#      aggregate decisions/sec just scales linearly with game count — the
#      pacing floor is the bottleneck there, not the lock. Sweep B pushes
#      game count high enough (measured single-threaded octogen throughput
#      on THIS box is ~31 decisions/sec at production TT — see
#      bench_results/stage4_octogen/single_thread_ceiling.txt — so demand
#      crosses that ceiling once N * 0.33/s approaches it, i.e. roughly
#      N ~= 90+) that aggregate demand actually exceeds the lock's
#      serialized supply, and the flattening becomes visible and
#      quantifiable — the same underlying g_kernel_lock ceiling Deliverable
#      2 describes, just measured at the game count where it actually bites.
#
# Usage:
#   bot_stress.sh [--port=8199] [--full-games=1,2,4,8] [--scale-games=1,8,32,96]
#                 [--secs=20] [--seats=7] [--spectators=2]
#
# Output: server/impls/native/bench_results/stage4_octogen/
#   full_stress.csv     one row per FULL-STRESS scale point
#   scaling.csv          one row per SCALING sweep point
#   single_thread_ceiling.txt   the standalone single-core octogen rate
#   summary.txt           human-readable summary of both sweeps + a PASS/FAIL
#                          gate check (bots actually progressed, no crash)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PORT=8199
FULL_GAMES="1,2,4,8"
SCALE_GAMES="1,8,32,96"
SECS=20
SEATS=7
SPECTATORS=2

for a in "$@"; do
    case "$a" in
        --port=*) PORT="${a#*=}" ;;
        --full-games=*) FULL_GAMES="${a#*=}" ;;
        --scale-games=*) SCALE_GAMES="${a#*=}" ;;
        --secs=*) SECS="${a#*=}" ;;
        --seats=*) SEATS="${a#*=}" ;;
        --spectators=*) SPECTATORS="${a#*=}" ;;
        *) echo "bot_stress.sh: unknown arg $a" >&2; exit 2 ;;
    esac
done

OUT_DIR="$SCRIPT_DIR/bench_results/stage4_octogen"
mkdir -p "$OUT_DIR"
FULL_CSV="$OUT_DIR/full_stress.csv"
SCALE_CSV="$OUT_DIR/scaling.csv"
CEILING_TXT="$OUT_DIR/single_thread_ceiling.txt"
SUMMARY="$OUT_DIR/summary.txt"

WORKDIR="$(mktemp -d /tmp/foolish_bot_stress.XXXXXX)"
PASS=true

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill -9 "$SRV_PID" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "== bot_stress.sh: workdir=$WORKDIR port=$PORT full_games=$FULL_GAMES scale_games=$SCALE_GAMES secs=$SECS seats=$SEATS spectators=$SPECTATORS =="

[ -x "$SCRIPT_DIR/foolish_server" ] || (cd "$SCRIPT_DIR" && make foolish_server >/dev/null)
[ -x "$SCRIPT_DIR/foolish_hammer" ] || (cd "$SCRIPT_DIR" && make foolish_hammer >/dev/null)

wait_for_health() {
    local h="$1" tries=0
    while [ $tries -lt 200 ]; do
        if curl -s -o /dev/null -w '%{http_code}' "$h/health" 2>/dev/null | grep -q 200; then return 0; fi
        sleep 0.05
        tries=$((tries + 1))
    done
    return 1
}

# CPU-seconds this process (all threads, live AND exited — see the "leader
# vs. sum-over-threads" note below) has consumed since it started: utime+
# stime off /proc/<pid>/stat (leader), in clock ticks, converted with
# getconf CLK_TCK (100 on Linux == centiseconds, the overwhelmingly common
# case — this script reads it rather than assuming). Reading the THREAD-
# GROUP LEADER's /proc/<pid>/stat (not summing /proc/<pid>/task/*/stat) is
# deliberate: Linux folds an EXITED thread's accumulated CPU time into the
# process-wide totals the leader's stat file reports, but a live per-thread
# stat file only ever shows that one thread's own time — under this
# workload's WS connect/reconnect churn (spectators and human clients both
# reconnect on any hiccup), summing only the CURRENTLY LIVE threads at one
# instant undercounts every thread that has already come and gone. Verified
# empirically before writing this script (a live run showed the leader's
# total consistently >= the live-thread sum, exactly as this reasoning
# predicts).
CLK_TCK="$(getconf CLK_TCK 2>/dev/null || echo 100)"
cpu_ticks() {
    local pid="$1" stat rest
    stat="$(cat "/proc/$pid/stat" 2>/dev/null)" || { echo 0; return; }
    rest="${stat##*) }"
    read -r -a f <<< "$rest"
    echo $(( ${f[11]:-0} + ${f[12]:-0} ))
}
NCORES="$(nproc)"

# Runs one foolish_hammer --mode=ws load window against a FRESH server
# (fresh --db in $WORKDIR, fresh port, so no state or CPU leaks between
# scale points), samples CPU + RSS concurrently, and appends one CSV row.
# `csv` = which file to append to; `label` = a short tag for stderr progress.
run_one() {
    local csv="$1" label="$2" games="$3" seats="$4" spectators="$5"
    local dbfile="$WORKDIR/${label}.db"
    local srvlog="$WORKDIR/${label}_server.log"
    local hmrlog="$WORKDIR/${label}_hammer.log"
    rm -f "$dbfile" "$dbfile-wal" "$dbfile-shm"

    "$SCRIPT_DIR/foolish_server" "$PORT" --db="$dbfile" --persist-interval-ms=100 \
        > "$srvlog" 2>&1 &
    SRV_PID=$!
    if ! wait_for_health "http://127.0.0.1:$PORT"; then
        echo "FAIL: [$label] server did not come up (see $srvlog)"; PASS=false
        kill -9 "$SRV_PID" >/dev/null 2>&1 || true; unset SRV_PID
        return
    fi
    echo "-- [$label] server up pid=$SRV_PID games=$games seats=$seats spectators=$spectators secs=$SECS"

    local t0_ticks t1_ticks rss_first rss_mean rss_peak rss_hwm
    t0_ticks=$(cpu_ticks "$SRV_PID")

    # mem_sample.sh runs for ~SECS+2s (a couple seconds of setup slack before
    # the timed hammer window starts) so the peak/mean cover the real load,
    # not just the idle startup.
    "$SCRIPT_DIR/mem_sample.sh" "$SRV_PID" "$((SECS + 6))" 0.25 \
        > "$WORKDIR/${label}_mem.csv" 2> "$WORKDIR/${label}_mem.err" &
    MEM_PID=$!

    "$SCRIPT_DIR/foolish_hammer" --host=127.0.0.1 --port="$PORT" \
        --games="$games" --seats="$seats" --spectators="$spectators" \
        --server-bot=octogen --secs="$SECS" --mode=ws \
        > "$hmrlog" 2>&1
    local hammer_status=$?

    wait "$MEM_PID" 2>/dev/null || true
    t1_ticks=$(cpu_ticks "$SRV_PID")

    kill -9 "$SRV_PID" >/dev/null 2>&1 || true
    wait "$SRV_PID" 2>/dev/null || true
    unset SRV_PID

    if [ "$hammer_status" -ne 0 ]; then
        echo "FAIL: [$label] foolish_hammer exited $hammer_status (see $hmrlog)"; PASS=false
        return
    fi

    local cpu_secs cpu_cores_avg
    cpu_secs=$(awk -v a="$t0_ticks" -v b="$t1_ticks" -v tck="$CLK_TCK" 'BEGIN{printf "%.3f", (b-a)/tck}')
    cpu_cores_avg=$(awk -v c="$cpu_secs" -v s="$SECS" 'BEGIN{printf "%.3f", c/s}')

    rss_first=$(awk -F, 'NR==2{print $2; exit}' "$WORKDIR/${label}_mem.csv" 2>/dev/null || echo 0)
    rss_mean=$(awk -F, 'NR>1{s+=$2;n++} END{if(n>0) printf "%.0f", s/n; else print 0}' "$WORKDIR/${label}_mem.csv" 2>/dev/null || echo 0)
    rss_peak=$(awk -F, 'NR>1{if($2>p)p=$2} END{print p+0}' "$WORKDIR/${label}_mem.csv" 2>/dev/null || echo 0)
    rss_hwm=$(grep -o 'VmHWM.*: *[0-9]*' "$WORKDIR/${label}_mem.err" 2>/dev/null | grep -o '[0-9]*' | tail -1)
    [ -z "${rss_hwm:-}" ] && rss_hwm=0

    local applied_per_s lat_mean lat_p99 spec_conns oct_dec oct_rate elapsed
    elapsed=$(grep 'wall clock (load phase):' "$hmrlog" | grep -o '[0-9.]*' | head -1)
    applied_per_s=$(grep 'applied(ok=true):' "$hmrlog" | grep -o '([0-9.]*[[:space:]]*applied/s)' | grep -o '[0-9.]*' | head -1)
    lat_mean=$(grep 'mean=' "$hmrlog" | grep -v spectator | grep -o 'mean=[0-9.]*' | head -1 | cut -d= -f2)
    lat_p99=$(grep 'mean=' "$hmrlog" | grep -v spectator | grep -o 'p99=[0-9.]*' | head -1 | cut -d= -f2)
    spec_conns=$(grep 'spectator connections:' "$hmrlog" | grep -o '[0-9]*' | head -1)
    oct_dec=$(grep 'octogen_decisions_summary:' "$hmrlog" | grep -o 'decisions=[0-9]*' | cut -d= -f2)
    oct_rate=$(grep 'octogen_decisions_summary:' "$hmrlog" | grep -o 'rate_per_s=[0-9.]*' | cut -d= -f2)

    echo "$label,$games,$seats,$spectators,${elapsed:-0},${applied_per_s:-0},${lat_mean:-0},${lat_p99:-0},${spec_conns:-0},${oct_dec:-0},${oct_rate:-0},$cpu_secs,$cpu_cores_avg,${rss_first:-0},${rss_mean:-0},${rss_peak:-0},${rss_hwm:-0}" \
        >> "$csv"
    echo "   applied/s=${applied_per_s:-0}  lat_mean_us=${lat_mean:-0}  lat_p99_us=${lat_p99:-0}  octogen_decisions=${oct_dec:-0} (${oct_rate:-0}/s)  cpu_cores_avg=$cpu_cores_avg/${NCORES}  rss_peak_kb=${rss_peak:-0}"

    if [ "${oct_dec:-0}" -le 0 ] 2>/dev/null; then
        echo "FAIL: [$label] zero octogen decisions — bots did not progress"; PASS=false
    fi
}

# ---------------------------------------------------------------------------
# Reference point: single-threaded, uncontended octogen throughput on THIS
# box, at the SAME production TT this server links (no CD_TT_BITS override —
# see Makefile) — the number that tells us where sweep B's game count needs
# to reach for aggregate demand to cross the lock's serialized supply.
# Reproducible: a small self-contained diagnostic linking the SAME kernel
# sources (KDIR/src/*.c), NOT foolish_server/foolish_hammer, run once and
# discarded (nothing to keep in-tree — same "one-off compiled with the same
# c/src sources" posture profile.sh's own callgrind runs take).
# ---------------------------------------------------------------------------
echo "== single-thread octogen ceiling (reference number for sweep B) =="
KDIR="../../../c"
DIAG_SRC="$WORKDIR/octogen_ceiling.c"
cat > "$DIAG_SRC" << 'EOF'
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "game.h"
#include "legal.h"
#include "bot_drive.h"
#include "bot_roster.h"
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec*1e-9; }
int main(void) {
    int ridx = bot_roster_find("octogen");
    const BotRosterEntry *e = bot_roster_at(ridx);
    int total_decisions = 0; double total_time = 0; int games = 20;
    for (int gi = 0; gi < games; gi++) {
        Game g; memset(&g, 0, sizeof g);
        g.num_players = 2;
        for (int i = 0; i < 2; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = (int8_t)e->strat;
            snprintf(g.players[i].player_id, sizeof g.players[i].player_id, "p%d", i);
        }
        game_set_seed((uint32_t)(gi + 1));
        start_game(&g);
        double t0 = now();
        for (int cycle = 0; cycle < 5000; cycle++) {
            BotDriveOut drv;
            if (bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &drv) < 0) break;
            total_decisions += drv.n;
            if (drv.ended >= 0 || drv.stop == BOT_STOP_NO_ELIGIBLE) break;
        }
        total_time += now() - t0;
    }
    printf("octogen_ceiling: decisions=%d wall_s=%.4f rate_per_s=%.2f\n",
           total_decisions, total_time, total_decisions / total_time);
    return 0;
}
EOF
DIAG_BIN="$WORKDIR/octogen_ceiling"
KERNEL_SRC=$(cd "$KDIR/src" && ls *.c | grep -v '^main_')
( cd "$KDIR/src" && cc -O2 -ffast-math -Wno-deprecated-declarations \
    -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK -I. \
    -o "$DIAG_BIN" "$DIAG_SRC" $KERNEL_SRC -lm -lpthread )
if [ -x "$DIAG_BIN" ]; then
    "$DIAG_BIN" | tee "$CEILING_TXT"
else
    echo "octogen_ceiling: build failed — skipping (sweep B will still run)" | tee "$CEILING_TXT"
fi
echo

# ---------------------------------------------------------------------------
# Sweep A: FULL-STRESS (the max-stress mix, --seats/--spectators as given)
# ---------------------------------------------------------------------------
echo "== sweep A: full-stress (1 octogen bot + $SEATS humans + $SPECTATORS spectators per game) =="
echo "label,games,seats,spectators,elapsed_s,applied_moves_per_s,lat_mean_us,lat_p99_us,spectator_conns,octogen_decisions,octogen_decisions_per_s,cpu_secs,cpu_cores_avg,rss_first_kb,rss_mean_kb,rss_peak_kb,rss_vmhwm_kb" \
    > "$FULL_CSV"
IFS=',' read -r -a FULL_ARR <<< "$FULL_GAMES"
for gcount in "${FULL_ARR[@]}"; do
    run_one "$FULL_CSV" "full_g${gcount}" "$gcount" "$SEATS" "$SPECTATORS"
done
echo

# ---------------------------------------------------------------------------
# Sweep B: SCALING (--seats=1, no spectators — maximize achievable game
# count within MAX_GAMES=256/MAX_USERS=512 so aggregate octogen-decision
# DEMAND can actually reach/exceed the single-thread ceiling above).
# ---------------------------------------------------------------------------
echo "== sweep B: scaling (1 octogen bot + 1 human per game, spectators=0) =="
echo "label,games,seats,spectators,elapsed_s,applied_moves_per_s,lat_mean_us,lat_p99_us,spectator_conns,octogen_decisions,octogen_decisions_per_s,cpu_secs,cpu_cores_avg,rss_first_kb,rss_mean_kb,rss_peak_kb,rss_vmhwm_kb" \
    > "$SCALE_CSV"
IFS=',' read -r -a SCALE_ARR <<< "$SCALE_GAMES"
for gcount in "${SCALE_ARR[@]}"; do
    run_one "$SCALE_CSV" "scale_g${gcount}" "$gcount" 1 0
done
echo

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
{
    echo "bot_stress.sh summary — $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host: $(uname -a)"
    echo "cores: $NCORES"
    echo
    echo "-- single-thread octogen ceiling (production TT, this box) --"
    cat "$CEILING_TXT"
    echo
    echo "-- sweep A: full-stress (seats=$SEATS, spectators=$SPECTATORS) --"
    column -s, -t "$FULL_CSV" 2>/dev/null || cat "$FULL_CSV"
    echo
    echo "-- sweep B: scaling (seats=1, spectators=0) --"
    column -s, -t "$SCALE_CSV" 2>/dev/null || cat "$SCALE_CSV"
    echo
    if $PASS; then
        echo "=== bot_stress.sh: PASS — octogen games progressed at every scale point, no server crash ==="
    else
        echo "=== bot_stress.sh: FAIL — see above ==="
    fi
} | tee "$SUMMARY"

echo
echo "== wrote $FULL_CSV, $SCALE_CSV, $CEILING_TXT, $SUMMARY =="

$PASS
