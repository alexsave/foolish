#!/bin/zsh
# Final cordite benchmark matrix. Usage: ./bench_cordite.sh <outdir>
# Runs the per-PC matrices vs all four opponents plus the bp-vs-bp control,
# then the mixed-pool ELO arena. Each block tees to its own log.
set -e
cd "$(dirname "$0")"
OUT=${1:-/tmp/cordite_bench}
mkdir -p "$OUT"

echo "=== $(date) cordite vs handwritten (1000/pc) ==="
./build/cnitro_eval --strategy=cordite --opp=handwritten \
    --players=2,3,4,5,6,7,8 --games=1000 --seed-start=910001 \
    2>/dev/null | tee "$OUT/vs_handwritten.txt"

echo "=== $(date) cordite vs espresso (1000/pc) ==="
./build/cnitro_eval --strategy=cordite --opp=espresso \
    --players=2,3,4,5,6,7,8 --games=1000 --seed-start=910001 \
    2>/dev/null | tee "$OUT/vs_espresso.txt"

echo "=== $(date) cordite vs random (1000/pc) ==="
./build/cnitro_eval --strategy=cordite --opp=random \
    --players=2,3,4,5,6,7,8 --games=1000 --seed-start=910001 \
    2>/dev/null | tee "$OUT/vs_random.txt"

echo "=== $(date) cordite vs blackpowder (400/pc) ==="
./build/cnitro_eval --strategy=cordite --opp=blackpowder \
    --players=2,3,4,5,6,7,8 --games=400 --seed-start=910001 \
    2>/dev/null | tee "$OUT/vs_blackpowder.txt"

echo "=== $(date) CONTROL bp vs blackpowder (400/pc) ==="
./build/cnitro_eval --strategy=blackpowder --opp=blackpowder \
    --players=2,3,4,5,6,7,8 --games=400 --seed-start=910001 \
    2>/dev/null | tee "$OUT/control_bp_vs_bp.txt"

echo "=== $(date) ELO arena (3000 games) ==="
./build/cnitro_elo --games=3000 \
    --pool=random,handwritten,espresso,robusta,firecracker,gunpowder,blackpowder,cordite \
    --pcs=2,3,4,5,6,7,8 --snapshot-every=500 \
    2>/dev/null | tee "$OUT/elo_arena.txt"

echo "=== $(date) done ==="
