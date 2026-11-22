#!/bin/bash
# test_citymix.sh: checks of citymix.py's seams, verify and song on synthetic signals. Seconds, self-cleaning.
#   test_citymix.sh              prints one PASS or FAIL line per check, exits 1 on any FAIL
# Environment: PYTHON (default python3), CITYMIX (the script under test, default the one beside this file;
# a copy with a deliberate break is how a check is mutation-checked), CITYMIX_TMP (where the scratch folder goes), KEEP=1
# (keep the scratch folder for inspection).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PYTHON="${PYTHON:-python3}"     # any python with numpy, scipy, soundfile and matplotlib
CM="${CITYMIX:-$HERE/citymix.py}"
T="$(mktemp -d "${CITYMIX_TMP:-${TMPDIR:-/tmp}}/citymix_test.XXXXXX")" || exit 2
[ -n "${KEEP:-}" ] || trap 'rm -rf "$T"' EXIT

"$PYTHON" -I - "$T" <<'PY' || { echo "FAIL generate synthetic inputs"; exit 1; }
import sys
import numpy as np
import soundfile as sf
from scipy import signal
SEED = 20261122                 # the only random source of the test signals
SR = 48000
T = sys.argv[1]
rng = np.random.RandomState(SEED)

def music(n, bass=0.0):
    t = np.arange(int(n * SR)) / SR
    y = np.zeros_like(t)
    for f in (220.0, 277.2, 329.6, 440.0):
        for k in range(1, 6):
            y += 0.06 / k * np.sin(2 * np.pi * f * k * t)
    y += rng.randn(len(t)) * 3e-5
    return t, y

def put(buf, t0, w):
    i = int(round(t0 * SR))
    if 0 <= i < len(buf):
        e = min(len(buf), i + len(w))
        buf[i:e] += w[:e - i]

def drums(beats, accent=None, n=20.0):
    r = np.random.RandomState(SEED + 1)
    buf = np.zeros(int(n * SR))
    tk = np.arange(int(0.4 * SR)) / SR
    kick = np.sin(2 * np.pi * (45 + 90 * np.exp(-tk / 0.03)) * tk) * np.exp(-tk / 0.09) * 0.6
    ts = np.arange(int(0.25 * SR)) / SR
    snare = r.randn(len(ts)) * np.exp(-ts / 0.05) * 0.35
    hat = np.diff(r.randn(len(ts) + 1)) * np.exp(-ts / 0.012) * 0.12
    for i, b in enumerate(beats):
        put(buf, b, kick if i % 2 == 0 else snare)
        put(buf, b + (beats[i + 1] - b) / 2 if i + 1 < len(beats) else b + 0.25, hat)
        if accent is not None and abs(b - accent) < 1e-6:
            put(buf, b, r.randn(len(ts)) * np.exp(-ts / 0.08) * 0.9)
    return buf

def wav(name, y):
    y = np.asarray(y)
    if y.ndim == 1:
        y = np.stack([y, y], axis=1)
    sf.write('%s/%s.wav' % (T, name), np.clip(y, -1, 1), SR, subtype='PCM_24')

def seamsfile(name, t=10.0):
    open('%s/%s.seams.tsv' % (T, name), 'w').write('idx\ttime\tprev_end\tstart\trole\tjoin_in\n2\t%.6f\t%.6f\t%.6f\tmain\tdownbeat\n' % (t, t - 0.01, t + 0.01))

def case(name, y):
    wav(name, y)
    seamsfile(name)

t, m = music(20.0)
case('music', m)
c = m.copy(); c[int(10 * SR):] += 0.0316            # a 1-sample -30 dBFS discontinuity that stays
case('click', c)
h = m.copy(); g = np.ones_like(t)
g[(t > 9.9) & (t < 10.1)] = 0; g = np.convolve(g, np.ones(240) / 240, 'same'); case('hole', h * g)
s = m.copy(); s[int(10 * SR):] *= 2.0; case('step6', s)
b = m.copy(); b += np.where(t < 10, 0.01, 0.1) * np.sin(2 * np.pi * 70 * t); case('lowstep', b)
k = np.arange(0, 20, 0.5)
P = music(20.0)[1] * 0.5       # a steady pad under the drums, as in music
case('drums_exact', drums(k) + P)
case('drums_accent', drums(k, accent=10.0) + P)
case('drums_shift', drums(np.concatenate((k[k < 10], 10.06 + 0.5 * np.arange(20)))) + P)
case('drums_tempo', drums(np.concatenate((k[k < 10], 10.1 + 60 / 150 * np.arange(25)))) + P)

# verify: two sources, an output built from them, a timeline that is true and one that is shifted by 30 ms
def am(n, seed):
    r = np.random.RandomState(seed)
    x = r.randn(int(n * SR))
    env = np.convolve(r.randn(int(n * SR)), np.ones(9600) / 9600, 'same')
    env = 0.3 + 6 * np.abs(env) / np.abs(env).max()
    return x * env * 0.1
s1, s2 = am(30, SEED + 2), am(30, SEED + 3)
sf.write('%s/src1.wav' % T, s1, SR, subtype='PCM_24'); sf.write('%s/src2.wav' % T, s2, SR, subtype='PCM_24')
r1 = s1[5 * SR:15 * SR]
r2 = signal.resample_poly(s2[3 * SR:13 * SR], 100, 103)      # rate 1.03: played faster
r3 = s1[20 * SR:int(20.6 * SR)]
out = np.concatenate((r1, r2, r3))
wav('vout', out)
e2 = 10 + len(r2) / SR
H = 'idx\tsong_start\tsong_end\tsrc_path\tsrc_in\tsrc_out\trate\tsemitones\tgain_db\trole\tjoin_in\n'
def tl(name, off):
    open('%s/%s.timeline.tsv' % (T, name), 'w').write(H +
        '1\t0.000000\t10.000000\t%s/src1.wav\t%.6f\t%.6f\t1.000000\t0.000000\t0.0\tmain\t\n' % (T, 5 + off, 15 + off) +
        '2\t10.000000\t%.6f\t%s/src2.wav\t3.000000\t13.000000\t1.030000\t0.000000\t0.0\tmain\t\n' % (e2, T) +
        '3\t%.6f\t%.6f\t%s/src1.wav\t20.000000\t20.600000\t1.000000\t0.000000\t0.0\tlayer\t\n' % (e2, e2 + 0.6, T))
tl('true', 0.0)
tl('shifted', 0.03)
PY

flagsof() { # table file -> flags of its row 1
    awk -F'\t' 'NR==1 { for (i = 1; i <= NF; i++) if ($i == "flags") c = i } NR==2 { print $c }' "$1"
}
fails=0
check() { # name, ok(0 = pass), detail
    if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1  ($3)"; fails=$((fails + 1)); fi
}
runseams() { "$PYTHON" -I "$CM" seams "$T/$1.wav" "$T/$1.seams.tsv" /dev/null "$T/out" "$1" > "$T/$1.tab" 2> "$T/$1.err"; }
has() { case ",$(flagsof "$T/$1.tab")," in *",$2,"*) return 0 ;; *) return 1 ;; esac; }

for n in music click hole step6 lowstep drums_exact drums_accent drums_shift drums_tempo; do
    runseams $n || { check "seams_runs_$n" 1 "$(tail -3 "$T/$n.err")"; }
done

has click CLICK; check click_flagged $? "flags $(flagsof "$T/click.tab")"
has music CLICK; [ $? = 1 ]; check click_not_on_clean_music $? "flags $(flagsof "$T/music.tab")"
has drums_exact CLICK; [ $? = 1 ]; check click_not_on_drum_loop $? "flags $(flagsof "$T/drums_exact.tab")"
has drums_accent CLICK; [ $? = 1 ]; check click_not_on_accented_hit_at_seam $? "flags $(flagsof "$T/drums_accent.tab")"
has hole HOLE; check hole_flagged_200ms $? "flags $(flagsof "$T/hole.tab")"
has music HOLE; [ $? = 1 ]; check hole_not_on_clean_music $? "flags $(flagsof "$T/music.tab")"
has drums_exact HOLE; [ $? = 1 ]; check hole_not_on_drum_loop $? "flags $(flagsof "$T/drums_exact.tab")"
has step6 STEP; check step_flagged_6db $? "flags $(flagsof "$T/step6.tab")"
has music STEP; [ $? = 1 ]; check step_not_on_clean_music $? "flags $(flagsof "$T/music.tab")"
has lowstep LOW; check low_flagged $? "flags $(flagsof "$T/lowstep.tab")"
has step6 LOW; [ $? = 1 ]; check low_not_on_whole_signal_step $? "flags $(flagsof "$T/step6.tab")"
has drums_shift BEAT; check beat_flagged_60ms $? "flags $(flagsof "$T/drums_shift.tab")"
has drums_exact BEAT; [ $? = 1 ]; check beat_not_on_exact_continuation $? "flags $(flagsof "$T/drums_exact.tab")"
has drums_tempo BEAT; [ $? = 1 ]; check beat_not_on_tempo_change $? "flags $(flagsof "$T/drums_tempo.tab")"
bpm=$(awk -F'\t' 'NR==1 { for (i = 1; i <= NF; i++) { if ($i == "bpm_a") a = i; if ($i == "bpm_b") b = i } } NR==2 { print $a, $b }' "$T/drums_tempo.tab")
awk -v s="$bpm" 'BEGIN { split(s, v, " "); exit !(v[1] != "-" && v[2] != "-" && (v[2] / v[1] > 1.03 || v[2] / v[1] < 0.97)) }'
check tempo_change_shows_in_bpm $? "bpm $bpm"

"$PYTHON" -I "$CM" verify "$T/vout.wav" "$T/true.timeline.tsv" > "$T/vt.txt" 2>&1; rc=$?
check verify_passes_true_timeline $rc "$(cat "$T/vt.txt")"
grep -q "1 hidden" "$T/vt.txt"; check verify_reports_short_row_hidden $? "$(cat "$T/vt.txt")"
"$PYTHON" -I "$CM" verify "$T/vout.wav" "$T/shifted.timeline.tsv" > "$T/vs.txt" 2>&1; rc=$?
[ $rc != 0 ] && grep -q "FAIL idx 1" "$T/vs.txt"; check verify_fails_30ms_shift $? "$(cat "$T/vs.txt")"

# an output past 900 s fails whatever the rows say: a header-only timeline on a 901 s file of silence (12 kHz keeps it small)
"$PYTHON" -I - "$T" <<'PY'
import sys, numpy as np, soundfile as sf
sf.write(sys.argv[1] + '/long.wav', np.zeros(12000 * 901, dtype='float32'), 12000, subtype='PCM_16')
PY
head -1 "$T/true.timeline.tsv" > "$T/empty.timeline.tsv"
"$PYTHON" -I "$CM" verify "$T/long.wav" "$T/empty.timeline.tsv" > "$T/vl.txt" 2>&1; rc=$?
[ $rc != 0 ] && grep -q "over 900" "$T/vl.txt"; check verify_fails_over_900s $? "$(cat "$T/vl.txt")"

"$PYTHON" -I "$CM" song "$T/vout.wav" "$T/click.seams.tsv" "$T/song.png" "test song" > "$T/song.out" 2>&1
[ -s "$T/song.png" ]; check song_png_exists $? "$(tail -3 "$T/song.out")"
[ -s "$T/out/click_s01.png" ] && [ -s "$T/out/click_seamsheet_1.png" ] && [ -s "$T/out/click_seams.tsv" ]
check seam_png_sheet_table_exist $? "$(ls "$T/out" | head)"
"$PYTHON" -I - "$T/out/click_s01.png" <<'PY'
import sys
from PIL import Image
w, h = Image.open(sys.argv[1]).size
sys.exit(0 if (w, h) == (1000, 520) else 1)
PY
check seam_png_is_1000x520 $? "size"

if [ "$fails" = 0 ]; then echo "test_citymix: all checks passed"; else echo "test_citymix: $fails FAILED"; fi
[ "$fails" = 0 ]
