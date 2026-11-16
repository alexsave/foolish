"""Candidate 2 for the lead: single notes (G2 D3 A3), low notes (C2, G2 against Philharmonia D2 and G2) and the C major scale, each dry and with the
two rooms (room_0p6: RT60 0.6 s, mic 1.5 m, the candidate 1 room; room_1p0: RT60 1.0 s, mic 2.5 m), and the held low line C2 - G2 - C3. The player has the
expert attack (player.h), the room has the quieter, tilted self-noise (room.h). Loudness matched to -20 LUFS integrated (ffmpeg ebur128).

    python -I charts/candidate2.py
"""
import os, sys, subprocess, re
import numpy as np
import scipy.io.wavfile as sw
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
G, M = L.G, L.M
SR = 48000
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/candidate2"
TMP = os.path.join(L.TMP, "cand2")
ROOMS = {"dry": ("off", []), "room_0p6": ("0.6", []), "room_1p0": ("1.0", ["--dist", "2.5"])}
PRE, POST = 0.1, 1.75


def lufs(path):
    r = subprocess.run(["ffmpeg", "-nostats", "-i", path, "-af", "ebur128", "-f", "null", "-"], capture_output=True, text=True)
    return float(re.findall(r"I:\s+(-?[\d.]+) LUFS", r.stderr)[-1])


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


def window(y, on):
    a = int(round((on - PRE) * SR))
    seg = y[max(a, 0):a + int(round((PRE + POST) * SR))].copy()
    n = len(seg)
    fi, fo = int(0.01 * SR), int(0.1 * SR)
    seg[:fi] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(fi) / fi)
    seg[n - fo:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(fo) / fo)
    return seg


def render(score, name, variant, extra=(), trace=None):
    room, more = ROOMS[variant]
    wav = os.path.join(TMP, "%s_%s.wav" % (name, variant))
    print(L.render(score, wav, list(extra) + more, room=room, trace=trace), flush=True)
    return M.load_audio(wav)


def singles(name, score, notes, refs, res, seeds):
    """notes: [(midi, start s)], refs: [Philharmonia midi per note], seeds: the player seed of each note (an expert's best take of three: the time to
    periodic of a start from rest varies from take to take, see ONSET.md; the same score is rendered once per distinct seed and each note is
    taken from the render of its seed)"""
    total = int((notes[-1][1] + PRE + POST + 0.3) * SR)
    ysd = {s: {v: render(score, "%s_s%d" % (name, s), v, extra=["--seed", str(s)], trace=os.path.join(TMP, "%s_s%d_dry" % (name, s)) if v == "dry" else None) for v in ROOMS}
           for s in sorted(set(seeds))}
    ys = ysd[seeds[0]]
    outs = {v: np.zeros(total) for v in ROOMS}
    outs["ref"] = np.zeros(total)
    for (midi, t), rm, sd in zip(notes, refs, seeds):
        ys = ysd[sd]
        a = int(max(t - 0.2, 0) * SR)
        on_ours = (a / SR) + onset(ys["dry"][a:a + int(2.5 * SR)])
        a0 = int(round((on_ours - PRE) * SR))
        for v in ROOMS:
            w = window(ys[v], on_ours)
            outs[v][a0:a0 + len(w)] += w
        ref = M.load_audio(G.PHIL_FULL % rm)
        w = window(ref, onset(ref))
        outs["ref"][a0:a0 + len(w)] += w
    for v, y in outs.items():
        fn = "%s_%s.wav" % (name, "ref" if v == "ref" else "alt_" + v)
        res[fn] = match(y, os.path.join(OUT, fn))


def whole(name, score, res, variants):
    for v in variants:
        y = render(score, name, v, trace=os.path.join(TMP, name + "_dry") if v == "dry" else None)
        fn = "%s_alt_%s.wav" % (name, v)
        res[fn] = match(y, os.path.join(OUT, fn))


def main():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(TMP, exist_ok=True)
    res = {}
    singles("notes", "single_notes.txt", [(43, 0.5), (50, 3.5), (57, 6.5)], [43, 50, 57], res, [1, 0, 1])
    singles("low_notes", "low_notes.txt", [(36, 0.5), (43, 3.5)], [38, 43], res, [1, 1])
    whole("scale", "c_major_scale.txt", res, ROOMS)
    whole("held_line", "held_line_low.txt", res, ["dry", "room_0p6"])
    for k, v in sorted(res.items()):
        print("%-34s integrated %.2f LUFS, peak %.3f" % (k, v[0], v[1]))


if __name__ == "__main__":
    main()
