"""Candidate 1 for the lead: single notes (EXPERT, room and dry) against the Philharmonia notes, and the C major scale (EXPERT room and dry,
NOVICE room). Everything loudness matched to -20 LUFS integrated (ffmpeg ebur128). Not for Alex: the lead blinds the files.

    python -I charts/candidate1.py
Single notes: window per note = 0.1 s before the detected onset to 1.75 s after it (2 percent of the maximum 10 ms RMS), 10 ms fade in and 100 ms
fade out, placed at the same times in files of the same length; the Philharmonia notes are the wav_full files (their own recorded room), ours get the room.
"""
import os, sys, subprocess, re
import numpy as np
import scipy.io.wavfile as sw
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import player_gates as G  # noqa: E402
M = G.M
SR = 48000
ROOT = G.ROOT
EXE = os.path.join(ROOT, "altcello")
SC = "/Users/alex/Music/foolish_cities_v5/alt_cello/scores"
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/candidate1"
TMP = G.TMP
NOTES = [(43, 0.5), (50, 3.5), (57, 6.5)]
PRE, POST = 0.1, 1.75


def render(score, wav, style, room, trace=None):
    cmd = ["nice", EXE, "render", os.path.join(SC, score), wav, "--style", style, "--room", str(room)]
    if trace: cmd += ["--trace", trace]
    r = subprocess.run(cmd, capture_output=True, text=True, check=True)
    print(r.stdout.strip(), flush=True)


def lufs(path):
    r = subprocess.run(["ffmpeg", "-nostats", "-i", path, "-af", "ebur128", "-f", "null", "-"], capture_output=True, text=True)
    m = re.findall(r"I:\s+(-?[\d.]+) LUFS", r.stderr)
    return float(m[-1])


def write(path, y):
    sw.write(path, SR, y.astype(np.float32))


def match(y, path, target=-20.0):
    write(path, y)
    g = 10 ** ((target - lufs(path)) / 20)
    write(path, y * g)
    return lufs(path), float(np.max(np.abs(y * g)))


def onset(y):
    env = M.rms_env(y)
    return int(np.argmax(env >= 0.02 * env.max())) * 0.01


def window(y, on, total):
    a = int(round((on - PRE) * SR))
    seg = y[max(a, 0):a + int(round((PRE + POST) * SR))].copy()
    n = len(seg)
    fi, fo = int(0.01 * SR), int(0.1 * SR)
    seg[:fi] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(fi) / fi)
    seg[n - fo:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(fo) / fo)
    return seg


def main():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(TMP, exist_ok=True)
    res = {}
    for nm, style, room in (("notes_alt_room", "expert", 0.6), ("notes_alt_dry", "expert", "off")):
        render("single_notes.txt", os.path.join(TMP, "cand_" + nm + ".wav"), style, room, os.path.join(TMP, "cand_" + nm) if room == "off" else None)
    render("single_notes.txt", os.path.join(TMP, "cand_notes_novice_dry.wav"), "novice", "off", os.path.join(TMP, "cand_notes_novice_dry"))
    dry = M.load_audio(os.path.join(TMP, "cand_notes_alt_dry.wav"))
    room = M.load_audio(os.path.join(TMP, "cand_notes_alt_room.wav"))
    total = int((NOTES[-1][1] + PRE + POST + 0.3) * SR)
    outs = {"notes_alt_room": np.zeros(total), "notes_alt_dry": np.zeros(total), "notes_ref": np.zeros(total)}
    for midi, t in NOTES:
        a = int(max(t - 0.2, 0) * SR)
        on_ours = (a / SR) + onset(dry[a:a + int(2.5 * SR)])
        a0 = int(round((on_ours - PRE) * SR))
        for nm, y in (("notes_alt_room", room), ("notes_alt_dry", dry)):
            w = window(y, on_ours, total)
            outs[nm][a0:a0 + len(w)] += w
        ref = M.load_audio(G.PHIL_FULL % midi)
        w = window(ref, onset(ref), total)
        outs["notes_ref"][a0:a0 + len(w)] += w
    for nm, y in outs.items():
        res[nm] = match(y, os.path.join(OUT, nm + ".wav"))
    for nm, style, room_ in (("scale_alt_room", "expert", 0.6), ("scale_alt_dry", "expert", "off"), ("scale_novice_room", "novice", 0.6)):
        wav = os.path.join(TMP, "cand_" + nm + ".wav")
        render("c_major_scale.txt", wav, style, room_, os.path.join(TMP, "cand_" + nm) if room_ == "off" else None)
        res[nm] = match(M.load_audio(wav), os.path.join(OUT, nm + ".wav"))
    for k, v in res.items():
        print("%-20s integrated %.2f LUFS, peak %.3f" % (k, v[0], v[1]))


if __name__ == "__main__":
    main()
