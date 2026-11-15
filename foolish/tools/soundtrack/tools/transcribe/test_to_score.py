#!/usr/bin/env python3
"""test_to_score.py - end-to-end test of to_score.py on a synthetic notes table, read back by the engine's own score reader.

What it does: writes a small notes TSV (bass on the beat, a top line with a note held over a bar line, a three-note chord,
two overlapping vocal notes of different lengths, notes slightly off the grid) at 120 BPM into a temp folder, runs to_score.py,
parses the output with a small parser here, then compiles a tiny C program that includes engine/render.h and engine/scoretxt.h
(as engine/test_tempo.c does) and reads the same score with score_txt. Each check prints PASS or FAIL.
Run: python3 -I test_to_score.py        Exit 0 = all pass, 1 = a check failed.
Environment: TO_SCORE overrides the script under test (used to mutation-check the checks); CC overrides the compiler (cc).
Failure modes: no C compiler or a changed engine API makes the engine checks fail; the synthetic expectations are written by hand
below, never taken from to_score.py.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.environ.get("TO_SCORE", os.path.join(HERE, "to_score.py"))
ENGINE = os.path.normpath(os.path.join(HERE, "..", "..", "engine"))
BPM = 120.0
SPB = 60.0 / BPM
GATE = 0.95
fails = []


def check(name, ok, detail=""):
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, "" if ok else "  " + detail))
    if not ok:
        fails.append(name)


# (stem, onset in beats as heard, duration in beats as heard, midi, transcription velocity)
# bars of 4 beats from beat 0 (t = 0), window 0 to 8 s = 4 bars.
SYN = [
    ("bass", 0, 1, 36, 80), ("bass", 1, 1, 36, 80), ("bass", 2, 1, 36, 80), ("bass", 3, 1, 36, 80),
    ("bass", 4, 2, 31, 60), ("bass", 6, 2, 38, 100),
    ("bass", 8.06, 4, 36, 80),          # a little late: quantises to beat 8
    ("bass", 12, 4, 33, 80),
    ("other", 0, 2, 64, 70),            # top line: E4
    ("other", 3, 3, 67, 70),            # G4, held across the bar line at beat 4 (sounds beats 3 to 6)
    ("other", 6, 1, 71, 70),            # B4
    ("other", 8, 2, 60, 70), ("other", 8.02, 2, 64, 70), ("other", 7.97, 2, 67, 70),   # a C major chord, slightly off the grid
    ("other", 12.07, 1.9, 71, 70),      # B4 late and 0.1 beats short: beat 12, length 2
    ("vocals", 0, 3, 57, 70),           # A3 for 3 beats
    ("vocals", 1, 1, 60, 70),           # C4 for 1 beat inside it: needs a second lane
]
# the same notes as the engine must read them back (beat, sounding beats before the 0.95 gate, midi pitch), per part
WANT = {
    "BASS": [(0, 1, 36), (1, 1, 36), (2, 1, 36), (3, 1, 36), (4, 2, 31), (6, 2, 38), (8, 4, 36), (12, 4, 33)],
    "LEAD": [(0, 2, 64), (3, 3, 67), (6, 1, 71), (8, 2, 67), (12, 2, 71)],
    "PAD": [(8, 2, 60), (8, 2, 64), (8, 2, 67)],
    "VOX": [(0, 3, 57), (1, 1, 60)],
}
TRANSPOSE = 2
ARGS_PARTS = ["--part", "bass=BASS", "--part", "other.top=LEAD", "--part", "other.chords=PAD", "--part", "vocals=VOX"]


def write_tsv(path):
    with open(path, "w") as f:
        f.write("stem\tonset_s\tduration_s\tmidi\tname\tvelocity\n")
        for st, on, du, m, v in SYN:
            f.write("%s\t%.4f\t%.4f\t%d\tx\t%d\n" % (st, on * SPB, du * SPB, m, v))


def parse(text):
    """Own small parser: returns (bpm, bars, {part: [lane: [bar token lists]]}, notes {part: [(beat, sound, pitch)]}, sums ok)."""
    NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
    bpm, bars, lanes, notes, sums = None, None, {}, {}, True
    for line in text.splitlines():
        if line.startswith("TEMPO"):
            bpm = float(line.split()[1])
        elif line.startswith("SECTION"):
            bars = int(re.search(r" BARS (\d+)", line).group(1))
        elif line.startswith("FX:"):
            pass
        elif ":" in line:
            part, rest = line.split(":", 1)
            lane_bars = [b.split() for b in rest.split("|")]
            lanes.setdefault(part, []).append(lane_bars)
            for bi, toks in enumerate(lane_bars):
                cur = 0.0
                for t in toks:
                    m = re.fullmatch(r"([^:@~]+):([0-9.]+)(?:~([0-9.]+))?(?:@([0-9.]+))?", t)
                    ln = float(m.group(2))
                    if m.group(1) != "r":
                        for p in m.group(1).split("+"):
                            if p == "x":
                                notes.setdefault(part, []).append((bi * 4 + cur, ln, -1))
                                continue
                            mm = re.fullmatch(r"([A-G])([#b]*)(-?\d+)", p)
                            pitch = NAMES[mm.group(1)] + sum(1 if c == "#" else -1 for c in mm.group(2)) + 12 * (int(mm.group(3)) + 1)
                            notes.setdefault(part, []).append((bi * 4 + cur, float(m.group(3)) if m.group(3) else ln, pitch))
                    cur += ln
                if abs(cur - 4.0) > 1e-9:
                    sums = False
    return bpm, bars, lanes, notes, sums


C_SRC = r'''
#define main engine_main
#include "render.h"
#undef main
#include "scoretxt.h"
void city_setup(void) {}
void city_loop_stub(void) {}
double city_loop(void) { return 0; }
void city_full(void) {}
static int tr_of[8];
static const char *names[] = {"BASS", "LEAD", "PAD", "VOX"};
static int map(const char *p, double *tp, double *vs, double *dp) {
    (void)dp; *tp = 0; *vs = 1;
    for (int i = 0; i < 4; i++) if (!strcmp(p, names[i])) return tr_of[i];
    return -1;
}
int main(int argc, char **argv) {
    for (int i = 0; i < 4; i++) tr_of[i] = syn_track(names[i], 0);
    score_txt(argv[1], map, 0);
    printf("bars %d\n", score_total_bars());
    for (int i = 0; i < g_nnote; i++) printf("note %s %.6f %.6f %.0f\n", names[g_note[i].tr == tr_of[0] ? 0 : g_note[i].tr == tr_of[1] ? 1 : g_note[i].tr == tr_of[2] ? 2 : 3], g_note[i].beat, g_note[i].len, g_note[i].pitch);
    return 0;
}
'''


def run_script(args, tmp, out):
    r = subprocess.run([sys.executable, "-I", SCRIPT] + args + ["--out", out], capture_output=True, text=True)
    return r


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tsv = os.path.join(tmp, "syn.tsv")
        out = os.path.join(tmp, "score.txt")
        write_tsv(tsv)
        r = run_script(["--notes", tsv, "--start", "0", "--end", "8", "--bpm", "120", "--downbeat", "0", "--name", "Syn",
                        "--key", "C", "--transpose", str(TRANSPOSE)] + ARGS_PARTS, tmp, out)
        check("script_runs", r.returncode == 0, r.stderr[-400:])
        if r.returncode != 0:
            return finish()
        text = open(out).read()
        bpm, bars, lanes, notes, sums = parse(text)
        check("tempo_and_bars", bpm == 120.0 and bars == 4, "tempo %s bars %s" % (bpm, bars))
        check("every_bar_sums_to_4", sums and all(len(l) == 4 for ls in lanes.values() for l in ls),
              "a bar does not sum to 4 beats or a lane lacks bars")
        want = {p: sorted((b, l, m + TRANSPOSE) for b, l, m in v) for p, v in WANT.items()}
        for p in want:
            check("parser_notes_" + p, sorted(notes.get(p, [])) == want[p] or
                  sorted((b, l, m) for b, l, m in notes.get(p, [])) == want[p], "%s != %s" % (sorted(notes.get(p, [])), want[p]))
        check("overlap_needs_two_lanes", len(lanes.get("VOX", [])) == 2 and len(lanes.get("BASS", [])) == 1,
              "VOX lanes %d" % len(lanes.get("VOX", [])))
        check("held_note_uses_tilde", "~" in text, "no ~ in output")
        check("held_note_tilde_exact", "A4:1~3" in text, "G4+2 = A4 held note should read A4:1~3")
        check("fx_line", re.search(r"^FX: .*bpm 120.*transposition 2", text, re.M) is not None)
        check("no_drums_in_score", "x:" not in text)
        # off-grid quantisation: the late bass note at 8.06 is exactly beat 8 in the output
        check("quantised_onset", "BASS: " in text and (8.0, 4.0, 38) in notes["BASS"], str(notes.get("BASS")))
        # a bad quantum must be refused
        r2 = run_script(["--notes", tsv, "--start", "0", "--end", "8", "--bpm", "120", "--downbeat", "0", "--name", "S", "--key", "C",
                         "--quantum", "0.3"] + ARGS_PARTS, tmp, out + "2")
        check("bad_quantum_refused", r2.returncode == 2, "exit %d" % r2.returncode)
        # drums path: formatting only, through a synthetic drums stem written with numpy
        engine_check(tmp, out)
        drum_check(tmp, tsv)
    return finish()


def engine_check(tmp, score):
    cfile = os.path.join(tmp, "readback.c")
    open(cfile, "w").write(C_SRC)
    exe = os.path.join(tmp, "readback")
    cc = os.environ.get("CC", "cc")
    r = subprocess.run([cc, "-O1", "-w", "-I", ENGINE, "-o", exe, cfile, "-lm"], capture_output=True, text=True)
    check("engine_reader_compiles", r.returncode == 0, r.stderr[-600:])
    if r.returncode != 0:
        return
    r = subprocess.run([exe, score], capture_output=True, text=True)
    check("engine_reader_runs", r.returncode == 0, r.stderr[-300:])
    check("engine_no_bar_warning", "warning: bar" not in r.stderr and "score:" not in r.stderr, r.stderr[-300:])
    got = {}
    nb = None
    for line in r.stdout.splitlines():
        p = line.split()
        if p[0] == "bars":
            nb = int(p[1])
        elif p[0] == "note":
            got.setdefault(p[1], []).append((float(p[2]), float(p[3]), float(p[4])))
    check("engine_bar_count", nb == 4, str(nb))
    for part, v in WANT.items():
        exp = sorted((b, l * GATE, m + TRANSPOSE) for b, l, m in v)
        g = sorted(got.get(part, []))
        ok = len(g) == len(exp) and all(abs(a[0] - e[0]) < 0.001 and abs(a[1] - e[1]) < 0.001 and a[2] == e[2] for a, e in zip(g, exp))
        check("engine_notes_" + part + "_count", len(g) == len(exp), "%d vs %d" % (len(g), len(exp)))
        check("engine_notes_" + part + "_onsets_pitches_lengths", ok, "%s != %s" % (g, exp))


def drum_check(tmp, tsv):
    """A synthetic drums stem: kicks (60 Hz bursts) on beats 1, 2, 3, 4 (loud, soft, loud, soft) and a hat burst (8 kHz) on beat 1.5."""
    import numpy as np
    from scipy.io import wavfile
    sr = 22050
    x = np.zeros(sr * 4, np.float32)
    rng = np.random.RandomState(1)

    def burst(t, f, ln, amp):
        n = int(ln * sr)
        a = int(t * sr)
        tt = np.arange(n) / sr
        x[a:a + n] += (amp * np.sin(2 * np.pi * f * tt) * np.exp(-tt * 40)).astype(np.float32)

    for k in range(4):
        burst((k + 1) * SPB, 60, 0.15, 0.9 if k % 2 == 0 else 0.4)
    burst(1.5 * SPB, 8000, 0.05, 0.5)
    sd = os.path.join(tmp, "stems")
    os.makedirs(sd)
    wavfile.write(os.path.join(sd, "drums.wav"), sr, x)
    out = os.path.join(tmp, "drums_score.txt")
    r = run_script(["--notes", tsv, "--start", "0", "--end", "8", "--bpm", "120", "--downbeat", "0", "--name", "D", "--key", "C",
                    "--drums", sd + ",low=KICK,high=HAT"], tmp, out)
    check("drums_script_runs", r.returncode == 0, r.stderr[-400:])
    if r.returncode != 0:
        return
    bpm, bars, lanes, notes, sums = parse(open(out).read())
    check("drums_bars_sum", sums and bars == 4, "")
    got = sorted(b for b, _, _ in notes.get("KICK", []))
    check("drums_kick_slots", got == [1.0, 2.0, 3.0, 4.0], str(got))
    txt = open(out).read()
    kl = [l for l in txt.splitlines() if l.startswith("KICK:")][0]
    check("drums_levels", kl == "KICK: r:1 x:0.25@1 r:0.75 x:0.25@0.7 r:0.75 x:0.25@1 r:0.75 | x:0.25@0.7 r:3.75 | r:4 | r:4", kl)
    check("drums_hat_present", "HAT:" in txt and "x:0.25@" in txt.split("HAT:")[1], txt[-200:])


def finish():
    print("FAIL test_to_score: %s" % ", ".join(fails) if fails else "PASS test_to_score")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
