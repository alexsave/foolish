"""The release window of a rendered note list: level (10 ms rms, 5 ms hop), the autocorrelation pitch and the bow contours from -0.15 s to +0.4 s around the
written end of every note, and the bump size.
  bump_db  = the largest rise of the level after the lift: max over t in [te + 0.03, te + 0.4] of (L(t) - min of L over [te, t])
  pitch_c  = the largest pitch excursion (cents re the median pitch over [te - 0.1, te]) over [te, te + 0.3] where L is within 30 dB of the sustain
    python -I charts/release_study.py WAV TRACE_PREFIX TE1,TE2,.. MIDI1,MIDI2,..  -> table on stdout
Also a library: windows(wav, ctrl_prefix, tes, midis) -> list of dicts (used by charts/onset2.py and charts/release_tab.py)."""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
M = L.M
SR = 48000


def env_db(y, t0, t1, win=0.01, hop=0.005):
    ts = np.arange(t0, t1, hop)
    out = []
    for t in ts:
        a = int(t * SR); w = int(win * SR)
        out.append(10 * np.log10(np.mean(y[a:a + w] ** 2) + 1e-18))
    return ts, np.array(out)


def pitch_at(y, t, fexp, win=0.06):
    a = int((t - win / 2) * SR); n = int(win * SR)
    x = y[a:a + n] - np.mean(y[a:a + n])
    if len(x) < n or np.max(np.abs(x)) < 1e-6: return np.nan
    ac = np.correlate(x, x, "full")[n - 1:]
    lo, hi = int(SR / (fexp * 1.06)), int(SR / (fexp / 1.06))
    k = lo + int(np.argmax(ac[lo:hi]))
    if 1 <= k < len(ac) - 1:
        a0, b0, c0 = ac[k - 1], ac[k], ac[k + 1]
        d = a0 - 2 * b0 + c0
        if d != 0: k = k + 0.5 * (a0 - c0) / d
    return SR / k


def windows(wav, prefix, tes, midis):
    y = M.load_audio(wav)
    c = np.fromfile(prefix + "_ctrl.f32", dtype=np.float32).reshape(-1, 11)
    res = []
    for te, midi in zip(tes, midis):
        ts, lv = env_db(y, te - 0.15, te + 0.4)
        sus = float(np.max(lv[ts < te - 0.02]))
        i0 = int(np.searchsorted(ts, te + 0.03))
        bump, tb = 0.0, 0.0
        for i in range(i0, len(ts)):
            m = float(np.min(lv[np.searchsorted(ts, te):i + 1]))
            if lv[i] - m > bump: bump, tb = lv[i] - m, ts[i] - te
        fexp = M.midi_hz(midi)
        pt = np.array([pitch_at(y, t, fexp) for t in ts])
        ref = np.nanmedian(pt[(ts >= te - 0.1) & (ts <= te)])
        cents = 1200 * np.log2(pt / ref)
        ok = (ts >= te) & (ts <= te + 0.3) & (lv > sus - 30)
        pe = float(np.nanmax(np.abs(cents[ok]))) if np.any(ok) else float("nan")
        j = np.arange(int((te - 0.15) * 1000), int((te + 0.4) * 1000))
        res.append(dict(te=te, midi=midi, t=ts - te, lev=lv - sus, pitch_c=cents, bump_db=bump, t_bump=tb, pitch_exc_c=pe, tc=(j / 1000.0) - te,
                        vB=c[j, 0], FB=c[j, 1], aF=c[j, 5], frac=c[j, 10]))
    return res


def main():
    wav, prefix = sys.argv[1], sys.argv[2]
    tes = [float(x) for x in sys.argv[3].split(",")]
    midis = [int(x) for x in sys.argv[4].split(",")]
    for r in windows(wav, prefix, tes, midis):
        i0 = int(np.argmin(np.abs(r["tc"])))
        lift = r["tc"][np.argmax(r["FB"][i0 - 100:] <= 0) + i0 - 100] if np.any(r["FB"][i0 - 100:] <= 0) else float("nan")
        lv = lambda d: float(np.interp(d, r["t"], r["lev"]))
        print("   level re sustain at +50/+100/+200/+300 ms: %.1f %.1f %.1f %.1f" % (lv(0.05), lv(0.1), lv(0.2), lv(0.3)))
        print("midi %d te %.2f: bump %.1f dB at +%.3f s, pitch excursion %.0f c; force zero at %+.3f s, speed at force zero %.4f m/s, aF at te %.2f" % (
            r["midi"], r["te"], r["bump_db"], r["t_bump"], r["pitch_exc_c"], lift, r["vB"][int(np.argmax(r["FB"][i0 - 100:] <= 0)) + i0 - 100] if np.isfinite(lift) else -1, r["aF"][i0]))
if __name__ == "__main__":
    main()
