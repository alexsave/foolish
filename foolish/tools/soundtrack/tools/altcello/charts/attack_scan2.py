"""Early level scan of the expert attack: per config (attv, sw lo/hi, attf) on G2 D3 A3 (scores/single_notes_1s.txt), 3 seeds: level re sustain (median 0.5-1.4 s) at
+20/+50/+100 ms after the onset (ours: the written start), audio time to periodic, 2-6 kHz attack-minus-sustain, HNR; Philharmonia same notes.
    python -I charts/attack_scan2.py  -> out/thermal/attack_scan2.txt"""
import os, sys, itertools
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
G, M = L.G, L.M
NOTES = [(43, 0.5), (50, 2.5), (57, 4.5)]


def lev(y, on):
    env = 20 * np.log10(M.rms_env(y) + 1e-12)
    a = int(round((on + 0.5) * 100)); b = int(round((on + 1.4) * 100))
    sus = float(np.median(env[a:b]))
    return [float(env[int(round((on + d) * 100))] - sus) for d in (0.02, 0.05, 0.1)]


def main():
    out = open(os.path.join(L.ROOT, "out", "thermal", "attack_scan2.txt"), "w")
    for midi, _ in NOTES:
        y = M.load_audio(G.PHIL_FULL % midi); on = G.onset_s(y)
        out.write("PHIL %d %s\n" % (midi, " ".join("%.1f" % x for x in lev(y, on)))); out.flush()
    cfgs = [("base", "--attv 0.4 --sw 0.25 0.45 --attf 0.4")] + [("v%.2f_s%.2f_f%.1f" % (v, s, f), "--attv %g --sw %g %g --attf %g" % (v, s, s * 1.5, f)) for v, s, f in itertools.product((0.6, 0.75, 0.9), (0.12, 0.2, 0.3), (0.4, 0.5))]
    if os.environ.get("SCAN") == "ACC":  # the bow acceleration decides the first 50 ms: scan it (Guettler cells span 0.25-8 m/s^2)
        cfgs = [("a%g_v%g" % (a, v), "--atta %g --attv %g --sw 0.2 0.3 --attf 0.4" % (a, v)) for a in (1, 2, 4, 8) for v in (0.6, 0.9)]
        out = open(os.path.join(L.ROOT, "out", "thermal", "attack_scan3.txt"), "w")
    for name, extra in cfgs:
        for seed in (0, 1, 2):
            wav = os.path.join(L.TMP, "as2_%s_%d.wav" % (name, seed))
            L.render("single_notes_1s.txt", wav, extra.split() + ["--seed", str(seed)])
            y = M.load_audio(wav)
            for midi, t in NOTES:
                seg, t0 = L.note_segment(y, t)
                on, tp = L.t_periodic(seg, M.midi_hz(midi), t_from=t0, tag="as2")
                g = G.gates_of(seg, midi, "as2_%d" % midi)
                out.write("ROW %s %d %d tper %.3f lev %s tilt %.1f hnr %.1f\n" % (name, midi, seed, tp, " ".join("%.1f" % x for x in lev(seg, t0)), g["att_tilt"], g["hnr"])); out.flush()
            os.remove(wav)
    out.close()


main()
