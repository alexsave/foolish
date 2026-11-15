#!/usr/bin/env python3
"""test_transcribe.py - end-to-end test of notes.sh, window_report.py and envelopes.py on deterministic synthetic audio.

What it does: writes a 6-bar, 120 BPM, 4/4 test set (starting 0.25 s into the file) into a temp directory: a bass line (C2 F2 G2 A2 F2 C2, two
notes per bar) and a melody (third and fifth of each bar's chord) as harmonic tones with a known envelope (linear
attack, linear decay to a sustain level, linear release), a drum stem (kick on steps 0 and 8, hats on steps 2, 6,
10, 14 of a 16-step bar) and a silent vocals stem. It then runs notes.sh on the bass, melody and mix, envelopes.py
and window_report.py, and checks: note recall (pitch exact, onset within 50 ms, at least 90 percent) for the bass
and the melody; attack, peak-to-sustain, sustain and release within 30 percent of the synthesised values (the
envelope check uses the ground-truth note list, because the transcribed note end is a model output); the chord
labels, the bass line names, and the low and high drum rows of the report.
Inputs: none (paths are temp files). Output: one PASS or FAIL line per named check, exit status 1 on any FAIL.
Example: tools/transcribe/test_transcribe.py   (needs the notes.sh venv; one basic-pitch run per stem, about 10 s)
Mutation hook: the environment variable TRANSCRIBE_MUTATE=<name> changes the synthesised audio or the expectation
  without touching the code under test, so each check can be seen to fail (names are in README.md).
Known failure modes: depends on basic-pitch transcribing clean harmonic tones, so a different model version can move
  the recall; the sustain level and times are measured on band-passed audio and carry a few ms of smoothing.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import envelopes as env  # noqa: E402

SR = 44100
MUT = os.environ.get("TRANSCRIBE_MUTATE", "")
BPM, BPB = 120.0, 4
BAR = BPB * 60.0 / BPM
NBARS = 6
ATK = 0.08 if MUT == "synth_attack" else 0.03
DEC = 0.10
SUS = 0.25 if MUT == "synth_sustain" else 0.5
REL = 0.28 if MUT == "synth_release" else 0.20
NOTE_DUR = 0.7
CHORDS = [("C",), ("F",), ("G",), ("Am",), ("F",), ("C",)]
BASS_MIDI = [36, 41, 43, 45, 41, 36]
MEL_MIDI = [(64, 67), (69, 72), (71, 74), (72, 76), (69, 72), (64, 67)]
LEAD = 0.25
KICK_STEPS = (0, 4) if MUT == "synth_drums" else (0, 8)


def tone(midi, dur, harm, bright=False):
    f = 440.0 * 2 ** ((midi - 69) / 12.0)
    n = int((dur + REL + 0.05) * SR)
    t = np.arange(n) / SR
    x = sum(a * np.sin(2 * np.pi * f * (k + 1) * t) for k, a in enumerate(harm))
    a_end = ATK
    if bright:  # 6th harmonic (outside the envelope band) that fades in 80 ms: brightness falls over the note
        x = x + 0.8 * np.sin(2 * np.pi * 6 * f * t) * np.exp(-t / 0.08)
    e = np.where(t < a_end, t / a_end, 1.0)
    d = np.clip((t - a_end) / DEC, 0, 1)
    e = np.where(t >= a_end, 1.0 - (1.0 - SUS) * d, e)
    r = np.clip((t - dur) / REL, 0, 1)
    e = np.where(t >= dur, SUS * (1.0 - r), e)
    return x * e


def place(buf, x, t0):
    i = int(round(t0 * SR))
    buf[i:i + len(x)] += x[:len(buf) - i]


def lcg_noise(n, seed=12345):
    out = np.empty(n)
    s = seed
    for i in range(n):
        s = (1664525 * s + 1013904223) & 0xFFFFFFFF
        out[i] = s / 2147483648.0 - 1.0
    return out


def synth(dirpath):
    total = int((LEAD + NBARS * BAR + 1.0) * SR)
    bass, mel, drums = (np.zeros(total) for _ in range(3))
    truth_b, truth_m = [], []
    for b in range(NBARS):
        for k in range(2):
            t0 = LEAD + b * BAR + k * 1.0
            place(bass, 0.35 * tone(BASS_MIDI[b], NOTE_DUR, [1.0, 0.5, 0.25], True), t0)
            truth_b.append((t0, t0 + NOTE_DUR, BASS_MIDI[b]))
            place(mel, 0.25 * tone(MEL_MIDI[b][k], NOTE_DUR, [1.0, 0.4, 0.2]), t0)
            truth_m.append((t0, t0 + NOTE_DUR, MEL_MIDI[b][k]))
    step = BAR / 16
    noise = lcg_noise(int(0.03 * SR))
    hat = np.diff(noise, 2, prepend=[0, 0]) * np.exp(-np.arange(len(noise)) / (0.006 * SR)) * 0.4
    kt = np.arange(int(0.15 * SR)) / SR
    kick = 0.8 * np.sin(2 * np.pi * (45 * kt + 40 * 0.03 * (1 - np.exp(-kt / 0.03)))) * np.exp(-kt / 0.05)
    for b in range(NBARS):
        for s in KICK_STEPS:
            place(drums, kick, LEAD + b * BAR + s * step)
        for s in (2, 6, 10, 14):
            place(drums, hat, LEAD + b * BAR + s * step)
    paths = {}
    os.makedirs(os.path.join(dirpath, "stems"))
    for name, x in (("bass", bass), ("other", mel), ("drums", drums), ("vocals", np.zeros(total))):
        p = os.path.join(dirpath, "stems", name + ".wav")
        wavfile.write(p, SR, (np.clip(x, -1, 1) * 32767).astype(np.int16))
        paths[name] = p
    mix = os.path.join(dirpath, "mix.wav")
    wavfile.write(mix, SR, (np.clip(bass + mel + 0.5 * drums, -1, 1) * 32767).astype(np.int16))
    paths["mix"] = mix
    return paths, truth_b, truth_m


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:], r.stderr[-2000:])
        raise SystemExit("command failed: " + " ".join(cmd))
    return r.stdout


def recall(truth, csvpath):
    notes = env.read_notes(csvpath)
    shift = 1 if MUT == "expect_pitch" else 0
    off = 0.100 if MUT == "expect_onset" else 0.0
    hit = 0
    for on, _, m in truth:
        ok = np.any((notes[:, 2] == m + shift) & (np.abs(notes[:, 0] - (on + off)) <= 0.05))
        hit += bool(ok)
    return hit / len(truth)


def expected_env():
    atk = 0.8 * 0.03                       # 10 to 90 percent of a linear 0.03 s ramp (expectation is fixed)
    p2s = (1.0 - 0.5 * 10 ** (1 / 20)) / 0.5 * 0.10
    sus_db = 20 * np.log10(0.5)
    rel = 0.9 * 0.20                       # linear release from the sustain: -20 dB is at 90 percent of the ramp
    return atk, p2s, sus_db, rel


def main():
    results = []

    def check(name, ok, detail):
        results.append((name, ok))
        print("%s %s: %s" % ("PASS" if ok else "FAIL", name, detail))

    tmp = tempfile.mkdtemp(prefix="transcribe_test_")
    paths, tb, tm = synth(tmp)
    ndir = os.path.join(tmp, "notes")
    for label, src in (("bass", paths["bass"]), ("other", paths["other"]), ("mix", paths["mix"])):
        run([os.path.join(HERE, "notes.sh"), src, ndir, label])
    csv = {k: os.path.join(ndir, k + "_basic_pitch.csv") for k in ("bass", "other", "mix")}
    rb, rm = recall(tb, csv["bass"]), recall(tm, csv["other"])
    check("notes_bass_recall", rb >= 0.9, "recall %.2f (pitch exact, onset within 50 ms)" % rb)
    check("notes_melody_recall", rm >= 0.9, "recall %.2f (pitch exact, onset within 50 ms)" % rm)

    # envelope check on the ground-truth note list
    truth_csv = os.path.join(tmp, "truth_bass.csv")
    with open(truth_csv, "w") as f:
        f.write("start_time_s,end_time_s,pitch_midi,velocity,pitch_bend\n")
        for on, off, m in tb:
            f.write("%.4f,%.4f,%d,80,0\n" % (on, off, m))
    out = run([os.path.join(HERE, "envelopes.py"), paths["bass"], truth_csv])
    got = {}
    for ln in out.splitlines():
        if ln.startswith("# "):
            p = ln[2:].split("\t")
            got[p[0]] = None if p[1] == "not measurable" else float(p[1].split()[1])
    exp = dict(zip(env.FIELDS, expected_env()))
    for fld in env.FIELDS[:4]:
        g, e = got.get(fld), exp[fld]
        ok = g is not None and abs(g - e) <= 0.3 * abs(e)
        check("envelope_" + fld, ok, "measured %s expected %.4f (tolerance 30 percent)" % (g, e))

    ca, cs, cr = got.get("centroid_attack_hz"), got.get("centroid_sustain_hz"), got.get("centroid_ratio")
    check("envelope_centroid_falls", ca is not None and cs is not None and cr is not None and ca > cs and cr > 1.3,
          "attack centroid %s Hz, sustain centroid %s Hz, ratio %s (brightness falls over each note, so ratio above 1.3)" % (ca, cs, cr))

    # window report
    md, tsv = os.path.join(tmp, "r.md"), os.path.join(tmp, "r.tsv")
    run([os.path.join(HERE, "window_report.py"), "--name", "synth", "--mix", paths["mix"], "--stems-dir",
         os.path.join(tmp, "stems"), "--csv-mix", csv["mix"], "--csv-bass", csv["bass"], "--csv-other", csv["other"],
         "--bpm", "120", "--beats-per-bar", "4", "--first-beat", str(LEAD), "--start", str(LEAD), "--end", str(LEAD + NBARS * BAR),
         "--out-md", md, "--out-tsv", tsv])
    txt = open(md).read()
    rows = {}
    for ln in txt.splitlines():
        if ln.startswith("| ") and ln.split("|")[1].strip().isdigit():
            rows.setdefault(ln.split("|")[1].strip(), []).append([c.strip() for c in ln.strip("|").split("|")])
    from window_report import midi_name  # noqa: E402
    ok = True
    for b in range(NBARS):
        bassrow = rows[str(b + 1)][0][1]
        ok = ok and bassrow.count(midi_name(BASS_MIDI[b]) + ":") >= 1
    check("report_bass_names", ok, "every bar's bass row names %s" % [midi_name(m) for m in BASS_MIDI])
    labels = []
    for b in range(NBARS):
        lab = [r for r in rows[str(b + 1)] if len(r) == 4 and r[2] in ("C", "F", "G", "Am", "Dm", "Em")]
        labels.append(lab[0][2] if lab else "?")
    want = [c[0] for c in CHORDS]
    check("report_chords", labels == want, "chords %s expected %s" % (labels, want))
    dr = [r for r in sum(rows.values(), []) if len(r) == 4 and set(r[1]) <= set("x.") and len(r[1]) == 16]
    kick_ok = sum(1 for r in dr if r[1] == "x.......x.......")
    hat_ok = sum(1 for r in dr if all(r[3][s] == "x" for s in (2, 6, 10, 14)))
    check("report_drum_low", kick_ok >= NBARS - 1, "%d of %d bars have the low row x.......x......." % (kick_ok, len(dr)))
    check("report_drum_high", hat_ok >= NBARS - 1, "%d of %d bars have high marks on steps 2 6 10 14" % (hat_ok, len(dr)))
    bad = [n for n, ok in results if not ok]
    print("RESULT: %s" % ("PASS" if not bad else "FAIL (%s)" % ", ".join(bad)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
