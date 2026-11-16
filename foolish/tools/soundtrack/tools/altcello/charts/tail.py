"""Release tail of single notes: the 10 ms rms level at +50, +100, +200, +300 ms after the end of the sound (the last frame within 3 dB of the median
level over 0.5-1.4 s after the onset), dB re that median. Ours: the raw renders of candidate2.py (dry, room_0p6, room_1p0); the Philharmonia notes: wav_full.
    python -I charts/tail.py"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
G, M = L.G, L.M
SR = L.SR
T = os.path.join(L.TMP, "cand2")


def tail(y, on):
    env = 20 * np.log10(M.rms_env(y) + 1e-12)
    a, b = int((on + 0.5) * 100), int((on + 1.4) * 100)
    med = float(np.median(env[a:b]))
    j = np.where(env >= med - 3.0)[0]
    jend = int(j[j >= a][-1])
    return [float(env[jend + k] - med) if jend + k < len(env) else np.nan for k in (5, 10, 20, 30)], jend * 0.01 - on


def main():
    rows = {}
    for midi, t in ((43, 0.5), (50, 3.5), (57, 6.5)):
        for v in ("dry", "room_0p6", "room_1p0"):
            y = M.load_audio(os.path.join(T, "notes_%s.wav" % v))
            a = int(max(t - 0.2, 0) * SR)
            seg = y[a:a + int(3.0 * SR)]
            on = G.onset_s(seg)
            rows.setdefault(v, []).append(tail(seg, on)[0])
        ref = M.load_audio(G.PHIL_FULL % midi)
        rows.setdefault("philharmonia", []).append(tail(ref, G.onset_s(ref))[0])
    for k, v in rows.items():
        m = np.nanmedian(np.array(v), axis=0)
        print("%-14s +50 ms %6.1f  +100 ms %6.1f  +200 ms %6.1f  +300 ms %6.1f dB re sustain (median of G2 D3 A3)" % ((k,) + tuple(m)))
        for mm, r in zip((43, 50, 57), v): print("    midi %d: %s" % (mm, " ".join("%6.1f" % x for x in r)))


main()
