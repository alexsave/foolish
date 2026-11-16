"""Render the single notes with a style and print the gate table against the Philharmonia notes.
    python -I charts/player_run.py single STYLE TAG [--seed N] [--damp X]   -> out/player/TAG.wav + table
"""
import os, sys, subprocess
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import player_gates as G  # noqa: E402
ROOT = G.ROOT
EXE = os.path.join(ROOT, "altcello")
SCORES = "/Users/alex/Music/foolish_cities_v5/alt_cello/scores"
NOTES = [(43, 0.5), (50, 3.5), (57, 6.5)]


def render(score, style, tag, extra=()):
    os.makedirs(G.TMP, exist_ok=True)
    wav = os.path.join(G.TMP, tag + ".wav")
    cmd = ["nice", EXE, "render", os.path.join(SCORES, score), wav, "--style", style, "--trace", os.path.join(G.TMP, tag), "--room", "off"] + [str(e) for e in extra]
    r = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return wav, r.stdout.strip()


def single(style, tag, extra=()):
    wav, msg = render("single_notes.txt", style, tag, extra)
    y = G.M.load_audio(wav)
    rows = {}
    for midi, t in NOTES:
        rows[midi] = G.alt_note_gates(y, t, midi, "%s_%d" % (tag, midi))
    return rows, msg


def fmt(v):
    return "  n/a" if v is None or not np.isfinite(v) else "%.3g" % v


def table(cols):
    """cols: list of (name, dict key->value)"""
    print("| gate | " + " | ".join(n for n, _ in cols) + " |")
    print("|---|" + "---|" * len(cols))
    for k in G.KEYS:
        print("| %s | " % G.LABEL[k] + " | ".join(fmt(c[k]) for _, c in cols) + " |")


def measure_only(tag):
    y = G.M.load_audio(os.path.join(G.TMP, tag + ".wav"))
    return {m: G.alt_note_gates(y, t, m, "%s_%d" % (tag, m)) for m, t in NOTES}


if __name__ == "__main__":
    style, tag = sys.argv[2], sys.argv[3]
    extra = sys.argv[4:]
    if sys.argv[1] == "measure":
        rows, msg = measure_only(tag), "re-measured %s.wav" % tag
    else:
        rows, msg = single(style, tag, extra)
    print(msg)
    ref = {m: G.ref_gates(m) for m, _ in NOTES}
    for m, _ in NOTES:
        print("== MIDI", m)
        table([("ALT " + style, rows[m]), ("Philharmonia", ref[m])])
    table([("ALT " + style + " median", G.median_table(list(rows.values()))), ("Philharmonia median (3 notes)", G.median_table(list(ref.values())))])
