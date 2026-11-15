# drum_events.py - kick, clap and hat events for a drum kit from a drums stem, at the stem's own band onsets.
#
# What it does: finds the onsets of the three drum streams exactly the way compare.py's drum pattern metric does (zero-phase
#   band-pass energy in dB: kick 30-120 Hz, clap 1.5-5 kHz, hat 7-11 kHz; rises of at least 6 dB (hat 4 dB) over 3 frames; the dB
#   floor is 45 dB under the stem's own peak in the band), so the events are the hits the judge counts. Each event's time is the
#   onset as detected (never moved to a grid). Its velocity is the band's peak level in the 50 ms after the onset, in dB below the
#   band's loudest hit, turned into an amplitude (vel = 10 ** (dB / 20) clipped to [floor, 1]); hits that the detector finds
#   within --min-gap-ms of an earlier hit of the same stream are not repeated (compare.py's picker already keeps 20 ms).
#   Each hit's length is the time to the next hit of the stream, capped at --max-len-s (the synths decay on their own).
# Inputs: --stem WAV --start S --end S --bpm B --out-prefix P [--vel-floor 0.05]
# Outputs: P_kick.txt, P_clap.txt, P_hat.txt (each "# NAME velmax_db X" then one "EVENTS NAME: beat,x,len,vel ..." line per at most 40 events;
#   the pitch is x, the part's drum pitch) and P_table.tsv (stream, time_s, band_peak_db, vel); on stdout one STATS line.
# How to run: python3 -I drum_events.py --stem drums.wav --start 0 --end 42 --bpm 160 --out-prefix out/dr
# Known failure modes: the kick stream also fires on sub-bass bleed in a Demucs drums stem and the clap stream on any
#   broadband transient (snare, crash); the streams are not exclusive, as in the judge.
# Randomness: none.
import argparse, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare as C

VEL_FLOOR = 0.05
MAX_LEN_S = 0.5
PER_LINE = 40
USE_PEAK_LEVEL = True        # the test mutates this: every hit gets velocity 1


def stream_hits(x, sr, name):
    lo, hi, win, rise = C.BANDS[name]
    e, hop = C.band_db(x, sr, lo, hi, win)
    fs = sr / hop
    e = np.maximum(e, e.max() - 45)
    d = np.zeros_like(e)
    d[3:] = np.maximum(e[3:] - e[:-3], 0)
    t, _ = C.pick_peaks(d, fs, min_dist_s=0.02, floor=rise)
    n50 = max(1, int(0.05 * fs))
    st = np.array([e[int(round(ti * fs)):int(round(ti * fs)) + n50 + 1].max() for ti in t]) if len(t) else np.zeros(0)
    return np.asarray(t, dtype=float), st


def build(x, sr, start, end, bpm, vel_floor=VEL_FLOOR, max_len_s=MAX_LEN_S):
    seg = x[int(round(start * sr)):int(round(end * sr))]
    out = {}
    for name in ('kick', 'clap', 'hat'):
        t, st = stream_hits(seg, sr, name)
        if len(t) == 0:
            out[name] = []
            continue
        vel = np.clip(10 ** ((st - st.max()) / 20), vel_floor, 1.0) if USE_PEAK_LEVEL else np.ones(len(t))
        nxt = np.append(t[1:], t[-1] + max_len_s)
        ln = np.minimum(nxt - t, max_len_s)
        out[name] = [(float(ti), float(l), float(v), float(s)) for ti, l, v, s in zip(t, ln, vel, st)]
    return out


def write(out, prefix, bpm):
    os.makedirs(os.path.dirname(os.path.abspath(prefix)), exist_ok=True)
    with open(prefix + '_table.tsv', 'w') as tb:
        tb.write('stream\ttime_s\tband_peak_db\tvel\n')
        for name, hits in out.items():
            vmax = max([h[3] for h in hits], default=0.0)
            with open('%s_%s.txt' % (prefix, name), 'w') as f:
                f.write('# %s velmax_db %.3f\n' % (name.upper(), vmax))
                for i in range(0, len(hits), PER_LINE):
                    f.write('EVENTS %s: ' % name.upper() + ' '.join('%.4f,x,%.4f,%.4f' % (h[0] * bpm / 60, h[1] * bpm / 60, h[2]) for h in hits[i:i + PER_LINE]) + '\n')
            for h in hits:
                tb.write('%s\t%.5f\t%.3f\t%.4f\n' % (name, h[0], h[3], h[2]))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--stem', required=True)
    ap.add_argument('--start', type=float, required=True)
    ap.add_argument('--end', type=float, required=True)
    ap.add_argument('--bpm', type=float, required=True)
    ap.add_argument('--out-prefix', required=True)
    ap.add_argument('--vel-floor', type=float, default=VEL_FLOOR)
    a = ap.parse_args(argv)
    x, sr = C.decode(a.stem, 44100)[0], 44100
    out = build(x, sr, a.start, a.end, a.bpm, a.vel_floor)
    write(out, a.out_prefix, a.bpm)
    print('STATS ' + ' '.join('%s=%d' % (k, len(v)) for k, v in out.items()))


if __name__ == '__main__':
    main()
