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

# per-band levels: a steady bed of three sines in each of the six bands (stable levels, no noise to flag by chance)
BEDHZ = {'sub': (30, 43, 55), 'bass': (80, 130, 200), 'lowmid': (300, 420, 530), 'mid': (800, 1300, 2000),
         'presence': (3000, 4500, 6000), 'air': (10000, 13000, 16000)}
pr = np.random.RandomState(SEED + 7)
PH = {k: pr.uniform(0, 2 * np.pi, 3) for k in BEDHZ}
def bed(gates=None):
    tt = np.arange(int(20 * SR)) / SR
    y = np.zeros_like(tt)
    for k, hz in BEDHZ.items():
        g = 1.0 if gates is None or k not in gates else gates[k]
        for f, ph in zip(hz, PH[k]):
            y += 0.01 * g * np.sin(2 * np.pi * f * tt + ph)
    return tt, y + np.random.RandomState(SEED + 8).randn(len(tt)) * 3e-5
def ramp(tt, a, b, w=0.02):        # 1 between a and b, raised-cosine edges of w seconds, 0 elsewhere
    up = np.clip((tt - (a - w / 2)) / w, 0, 1); dn = np.clip(((b + w / 2) - tt) / w, 0, 1)
    return 0.5 - 0.5 * np.cos(np.pi * np.minimum(up, dn))
tt, y = bed(); case('band_clean', y)
# two sources of a 100 Hz line summed over 1 s (9.5-10.5), one before and one after: the level is the same either side
tt, y = bed(); one = 0.1 * np.sin(2 * np.pi * 100 * tt)
case('band_bump', y + one * (ramp(tt, 0, 10.5, 0.05) + ramp(tt, 9.5, 20, 0.05)))
# everything above 8 kHz disappears at the seam
tt, y = bed(); tt, y0 = bed({'air': 0.0})
case('band_air', y0 + (y - y0) * (1 - np.clip((tt - 9.98) / 0.04, 0, 1)))
# a 600 ms silence of only the sub and bass lines (9.7-10.3)
tt, y = bed()
tt, ylow = bed({k: 0.0 for k in ('lowmid', 'mid', 'presence', 'air')})
case('band_dip', y - ylow * ramp(tt, 9.7, 10.3, 0.02))

# melody: a 1 s lead-in, then 4 bars of 2 s (beat 0.5 s): a bass sine, a triad of three sines (C major, then A minor) and a melody
# sine that changes every half bar
tt = np.arange(int(12 * SR)) / SR
def tone(f, a, b, amp, ramp=0.005):
    g = np.clip((tt - a) / ramp, 0, 1) * np.clip((b - tt) / ramp, 0, 1)
    return amp * g * np.sin(2 * np.pi * f * tt)
mel = np.zeros_like(tt)
for bar, (bass, chord) in enumerate(((41.2, (130.81, 164.81, 196.0)),) * 2 + ((55.0, (110.0, 130.81, 164.81)),) * 2):
    a, b = 1.0 + 2.0 * bar, 3.0 + 2.0 * bar
    mel += tone(bass, a, b, 0.3)
    for f in chord:
        mel += tone(f, a, b, 0.1)
for k, m in enumerate((74, 77, 71, 74, 79, 77, 83, 86)):
    mel += tone(440.0 * 2 ** ((m - 69) / 12), 1.0 + k, 2.0 + k, 0.1)
wav('melody', mel)

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

bandflags() { # case, band -> flags of that band's row of the seam's _bands.tsv
    awk -F'\t' -v b="$2" '$4 == b { print $10 }' "$T/out/$1_bands.tsv"
}
allbands() { for b in sub bass lowmid mid presence air; do printf '%s=%s ' "$b" "$(bandflags "$1" "$b")"; done; }
onlyflag() { # case, band, flag: that band has exactly this flag and every other band has none
    local b
    [ "$(bandflags "$1" "$2")" = "$3" ] || return 1
    for b in sub bass lowmid mid presence air; do [ "$b" = "$2" ] || [ "$(bandflags "$1" "$b")" = "-" ] || return 1; done
}
for n in band_clean band_bump band_air band_dip; do
    runseams $n || { check "seams_runs_$n" 1 "$(tail -3 "$T/$n.err")"; }
done
onlyflag band_bump bass BUMP; check band_bump_two_sources_flags_bass_only $? "$(allbands band_bump)"
onlyflag band_air air STEP; check band_air_step_flags_air_only $? "$(allbands band_air)"
d_ok=1; for b in sub bass; do case "$(bandflags band_dip $b)" in *DIP*) d_ok=0 ;; esac; done
for b in lowmid mid presence air; do [ "$(bandflags band_dip $b)" = "-" ] || d_ok=1; done
check band_dip_below_250_flags_sub_or_bass_only $d_ok "$(allbands band_dip)"
all_ok=0; for b in sub bass lowmid mid presence air; do [ "$(bandflags band_clean $b)" = "-" ] || all_ok=1; done
grep -Eq '^BANDS 01 0:10\.0 \| sub -?[0-9]+>-?[0-9]+ \| bass .* \| air -?[0-9]+>-?[0-9]+ \| -$' "$T/band_clean.tab" || all_ok=1
check band_clean_signal_flags_nothing $all_ok "$(allbands band_clean) / $(grep '^BANDS' "$T/band_clean.tab")"
rows=$(awk -F'\t' 'NR > 1 && $1 == "01" { n++ } END { print n + 0 }' "$T/out/band_bump_bands.tsv")
names=$(awk -F'\t' 'NR > 1 { print $4 }' "$T/out/band_bump_bands.tsv" | tr '\n' ' ')
[ "$rows" = 6 ] && [ "$names" = "sub bass lowmid mid presence air " ]; check bands_tsv_six_rows_per_seam $? "$rows rows: $names"

"$PYTHON" -I "$CM" melody "$T/melody.wav" 1.0 0.5 4 > "$T/melody.out" 2> "$T/melody.err"
want=$(printf 'bar\tstart\tbass\ttriad\thalf1\thalf2\n1\t1.000\tE1\tC\tD5(74)\tF5(77)\n2\t3.000\tE1\tC\tB4(71)\tD5(74)\n3\t5.000\tA1\tAm\tG5(79)\tF5(77)\n4\t7.000\tA1\tAm\tB5(83)\tD6(86)\n')
[ "$(cat "$T/melody.out")" = "$want" ]; check melody_reads_back_bass_triad_and_half_bar_pitches $? "$(cat "$T/melody.out" "$T/melody.err" | tail -8)"

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
sys.exit(0 if (w, h) == (1000, 740) else 1)
PY
check seam_png_is_1000x740_taller_than_before $? "size"

if [ "$fails" = 0 ]; then echo "test_citymix: all checks passed"; else echo "test_citymix: $fails FAILED"; fi
[ "$fails" = 0 ]
