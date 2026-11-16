"""Candidate 4 for the lead: the candidate 3 file set rendered with the Guettler-wedge attack and the smooth expert release (iteration 2): single notes (G2 D3 A3), low notes
(C2, G2 against Philharmonia D2 and G2), the C major scale, the held low line; dry and room_0p6. The player seed of each single note is the best of three takes by the audio
time to periodic (out/wedge/final_NAME.txt of charts/wedge_final.py, seeds 0 1 2). Loudness -20 LUFS. Writes KEY.txt.

    python -I charts/candidate4.py FINAL_NAME [OUTDIR]
"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import candidate2 as C  # noqa: E402
import onset_lib as L  # noqa: E402

OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/candidate4"
LAB = {"G2": "43", "D3": "50", "A3": "57", "C2": "36", "G2c": "43c"}


def final_rows(name):
    rows = {}
    for l in open(os.path.join(L.ROOT, "out", "wedge", "final_%s.txt" % name)):
        q = l.split()
        if q and q[0] == "ROW" and int(q[3]) <= 2:
            rows.setdefault(q[2], {})[int(q[3])] = [float(q[5])] + [float(x) for x in q[7:10]]
    return rows


def best_seeds(rows):
    return {LAB[n]: min(d, key=lambda s: (d[s][0] if d[s][0] >= 0 else 99.0, s)) for n, d in rows.items()}


def med(rows, n, k):
    return float(np.median([v[k] for v in rows[n].values()]))


KEY = """ALT-CELLO candidate 4 (for the lead to check, not for Alex). All files 48 kHz mono float32, integrated loudness matched to -20 LUFS (ffmpeg ebur128).
Made by charts/candidate4.py in the alt-cello worktree (commit named in the report). Same file set as candidate 3.

What changed since candidate 3 (the player only: no change to the strings, bow model, body, bridge, room or loudness)
  - Guettler-wedge attack (expert): the bow force lands 20 ms before the motion at a force proportional to the bow acceleration (per string, generated table in instrument_cello.c:
    1.5 m/s^2 on every string, force C 1.0 N, G 0.9 N, D and A 1.3 N), the bow reaches 0.85 of its plateau speed in 0.05-0.08 s, the force is held 0.1 s and then relaxes smoothly (0.05-0.15 s)
    to the sustain position; the 20-80 percent band clamp applies after that. Candidate 3: 0.5 m/s^2 to 0.4 of the speed, force low, the first 50 ms 8-17 dB under the real notes.
  - expert release: the force and the speed already ease off with the bow moving at the lift (unchanged); what changed is the left hand: it keeps the note stopped for 0.3 s after the lift and
    lets go over 0.24-0.45 s (candidate 3: it eased off at once, and the finger lift made the string ring up again 13-17 dB 0.1-0.2 s after the lift). The novice keeps its abrupt stop.
  - the player seed of each single note is the best of three takes by the audio time to periodic: notes G2 D3 A3 use seeds %s, low notes C2 G2 use seeds %s (the scale and the held line use seed 0).

Rooms (the same for every file with the suffix)
  dry        radiated pressure only
  room_0p6   RT60 0.6 s, cardioid microphone 1.5 m

Single notes G2 (MIDI 43, stopped on the C string: with vibrato the player never plays an open string), D3 (50, on the G string), A3 (57, on the D string), forte (vel 0.85), vibrato on;
windows 0.1 s before the onset to 1.75 s after it, 10 ms fade in, 100 ms fade out; notes at 0.4, 3.4, 6.4 s
  notes_alt_dry.wav  notes_alt_room_0p6.wav   ALT-CELLO, expert, thermal bow
  notes_ref.wav                               the three Philharmonia solo cello notes (wav_full/ph_cello_43, _50, _57), onsets aligned to ours

Low notes C2 (36) and G2 (43, stopped on the C string), forte, vibrato on (the open C string takes none); windows as above, at 0.4 and 3.4 s
  low_notes_alt_dry.wav  low_notes_alt_room_0p6.wav
  low_notes_ref.wav     Philharmonia: there is no C2 in wav_full (the lowest is MIDI 38, D2), so the first slot holds ph_cello_38 (D2) and the second ph_cello_43 (G2)

C major scale C3-C4, 72 BPM quarters, legato, vel 0.7, vibrato on, 1 s rest, then the same scale with vibrato off. There is NO Philharmonia scale.
  scale_alt_dry.wav  scale_alt_room_0p6.wav

Held low line C2 - G2 - C3, whole notes at 72 BPM (3.333 s each), legato bow changes, vel 0.7, vibrato on, player seed 0
  held_line_alt_dry.wav  held_line_alt_room_0p6.wav

Measured (three player seeds 0 1 2, dry, medians; candidate 3 in brackets from charts/wedge_final.py): level re sustain at 50 ms G2 %s dB (%s), D3 %s (%s), A3 %s (%s); Philharmonia G2 -6.9, D3 -3.5, A3 -2.7.
Audio time to periodic G2 %s s (%s), D3 %s (%s), A3 %s (%s), C2 %s (%s).

Known faults: C2 (open C string) still starts in 0.2-0.8 s depending on the take (its level at 50 ms is 10 dB higher than candidate 3, the Helmholtz motion forms late); the first 50 ms of G2 are still
3-4 dB under the real note; the low notes are still 11-20 dB too bright in 1-3 kHz; the finger lift after the note (now 0.3 s late) can still be heard as a small rise if the note is cut late.
"""


def main():
    name = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else OUT
    C.OUT = out
    C.TMP = os.path.join(L.TMP, "cand4")
    C.ROOMS = {k: v for k, v in C.ROOMS.items() if k != "room_1p0"}
    os.makedirs(out, exist_ok=True)
    os.makedirs(C.TMP, exist_ok=True)
    rows = final_rows(name)
    sd = best_seeds(rows)
    print("seeds", sd)
    res = {}
    C.singles("notes", "single_notes.txt", [(43, 0.5), (50, 3.5), (57, 6.5)], [43, 50, 57], res, [sd["43"], sd["50"], sd["57"]])
    C.singles("low_notes", "low_notes.txt", [(36, 0.5), (43, 3.5)], [38, 43], res, [sd["36"], sd["43c"]])
    C.whole("scale", "c_major_scale.txt", res, C.ROOMS)
    C.whole("held_line", "held_line_low.txt", res, ["dry", "room_0p6"])
    c3 = final_rows("c3")
    f = lambda r, n, k: "%.1f" % med(r, n, k)
    g = lambda r, n: "%.2f" % np.median([v[0] for v in r[n].values()])
    key = KEY % (" ".join(str(sd[k]) for k in ("43", "50", "57")), " ".join(str(sd[k]) for k in ("36", "43c")),
                 f(rows, "G2", 2), f(c3, "G2", 2), f(rows, "D3", 2), f(c3, "D3", 2), f(rows, "A3", 2), f(c3, "A3", 2),
                 g(rows, "G2"), g(c3, "G2"), g(rows, "D3"), g(c3, "D3"), g(rows, "A3"), g(c3, "A3"), g(rows, "C2"), g(c3, "C2"))
    open(os.path.join(out, "KEY.txt"), "w").write(key)
    for k, v in sorted(res.items()):
        print("%-34s integrated %.2f LUFS, peak %.3f" % (k, v[0], v[1]))
    return sd, res


if __name__ == "__main__":
    main()
