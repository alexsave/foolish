#!/bin/bash
# test_seoul11_repro.sh: build, determinism, section energy, and loudness checks for seoul11_repro.c.
set -euo pipefail
D="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
SRC="${REPRO_C:-$D/seoul11_repro.c}"
clang -O2 -Wall -Werror -I"$D/../engine" -o "$T/repro" "$SRC" -lm 2>"$T/cc.log"
[ ! -s "$T/cc.log" ] || { cat "$T/cc.log"; echo "FAIL build warnings"; exit 1; }
"$T/repro" "$T/a.wav" 2>"$T/run.log"
"$T/repro" "$T/b.wav" 2>/dev/null
cmp -s "$T/a.wav" "$T/b.wav" && echo "PASS byte-identical determinism" || { echo "FAIL determinism"; exit 1; }
python3 - "$T/a.wav" <<'PY'
import sys, subprocess, numpy as np
wav = sys.argv[1]
d = subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', '-i', wav, '-f', 'f32le', '-ar', '48000', '-ac', '2', '-'], capture_output=True, check=True)
y = np.frombuffer(d.stdout, dtype='<f4').reshape(-1, 2).astype(np.float64)
SR = 48000
dur = len(y) / SR
assert abs(dur - 122.5) < 0.05, f'dur {dur:.2f} s'
pk = 20 * np.log10(np.abs(y).max() + 1e-12)
assert -1.5 <= pk <= -0.9, f'peak {pk:.2f} dBFS'
# All 6 musical sections must be active (intro, drop1, chill, breakdown, build, final drop)
sections = [('intro', 2, 14), ('drop1', 20, 38), ('chill', 45, 62), ('breakdown', 68, 82), ('build', 86, 104), ('final_drop', 109, 118)]
rms = {}
for name, a, b in sections:
    s = y[a*SR:b*SR]
    r = 20 * np.log10(np.sqrt(np.mean(s*s)) + 1e-12)
    assert r > -26.0, f'{name} too quiet ({r:.1f} dBFS)'
    rms[name] = r
# Drops must be meaningfully louder than the chill verse and dreamy breakdown
assert rms['drop1'] > rms['chill'] + 2.0, f"drop1 {rms['drop1']:.1f} not louder than chill {rms['chill']:.1f}"
assert rms['final_drop'] > rms['breakdown'] + 2.0, f"final_drop {rms['final_drop']:.1f} not louder than breakdown {rms['breakdown']:.1f}"
print(f"PASS audio sanity - dur {dur:.2f}s, peak {pk:.2f} dBFS, drop1 {rms['drop1']:.1f} vs chill {rms['chill']:.1f} dBFS")
PY
