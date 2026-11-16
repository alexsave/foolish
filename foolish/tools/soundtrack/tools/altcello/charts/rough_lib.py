"""Low-note roughness work (iteration 1): the contact trace of a played note (rough_tool) and the audio measures.
Notes as in rich_lib (D2, G2c, D3, A3; expert, forte, 1.5 s, dry, vibrato on unless asked). Trace analysis windows: 1.0-1.9 s (0.5 s after the note start)."""
import os, sys, subprocess
import numpy as np
import scipy.signal as ss
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rich_lib as R  # noqa: E402
import onset_lib as L  # noqa: E402
import diagnose as D  # noqa: E402
G, M = L.G, L.M
ROOT = L.ROOT
OUT = os.path.join(ROOT, "out", "rough")
FS = 96000
THR = 1e-2          # m/s, the thermal slip threshold of analysis.h
MINDUR = 0.00015    # s
NC = ["vbow", "vrel", "fric", "tau", "z", "bridge"]
os.makedirs(OUT, exist_ok=True)
R.NOTES["G2o"] = (43, "G", "g2o")      # the same pitch on the open G string (the Philharmonia G2 may be this one)
R.NOTES["G2onv"] = (43, "G", "g2onv")  # ... without vibrato


def nfile(key):
    return os.path.join(R.OUT, R.NOTES[key][2] + ".txt")


def run_trace(key, seed, tag, knobs="", vib=True, extra=()):
    pre = os.path.join(OUT, "%s_%s_s%d%s" % (tag, R.NOTES[key][2], seed, "" if vib else "_nv"))
    if not os.path.exists(pre + ".f32"):
        env = dict(os.environ)
        env["ALTCELLO_KNOBS"] = knobs
        cmd = ["nice", os.path.join(ROOT, "rough_tool"), nfile(key), pre, "--seed", str(seed)] + (["--vib", "off"] if not vib else []) + [str(e) for e in extra]
        subprocess.run(cmd, check=True, env=env, capture_output=True)
    a = np.fromfile(pre + ".f32", dtype=np.float32).reshape(-1, len(NC))
    return {n: a[:, i].astype(np.float64) for i, n in enumerate(NC)}


def runs(mask):
    d = np.diff(np.concatenate([[0], mask.astype(np.int8), [0]]))
    return np.where(d == 1)[0], np.where(d == -1)[0]


def trace_stats(tr, f0, t0=1.0, t1=1.9):
    """slip pattern of the contact trace. Periods are anchored on the LARGEST slip: the next main slip is the largest slip that starts 0.7-1.3 nominal
    periods later (so a note with two big slips per period, a multiple slipping, keeps one main slip and counts the other as an extra slip);
    extra = slips of at least 0.15 of the main slip's displacement inside a period (not the main one), per period."""
    a, b = int(t0 * FS), int(t1 * FS)
    v, f, tau = tr["vrel"][a:b], tr["fric"][a:b], tr["tau"][a:b]
    T0 = FS / f0
    s, e = runs(np.abs(v) > THR)
    keep = (e - s) >= MINDUR * FS
    s, e = s[keep], e[keep]
    nan = dict(nslip=len(s), jit=np.nan, extra=np.nan, slipfrac=np.nan, slipfrac_sd=np.nan, creep=np.nan, chatter=np.nan, tau_sw=np.nan, tau_mean=np.nan, T=np.nan, main=0, dsd=np.nan, big2=np.nan, pm=np.nan)
    if len(s) < 4:
        return nan
    disp = np.array([np.sum(np.abs(v[i:j])) / FS for i, j in zip(s, e)])
    first = np.where(s < s[0] + 1.2 * T0)[0]
    cur = first[np.argmax(disp[first])]
    mains = [cur]
    while True:
        c = np.where((s > s[cur] + 0.7 * T0) & (s < s[cur] + 1.3 * T0))[0]
        if not len(c): break
        cur = c[np.argmax(disp[c])]
        mains.append(cur)
    mains = np.array(mains)
    if len(mains) < 5:
        return nan
    ms = s[mains]
    T = np.diff(ms) / FS
    jit = 100.0 * np.std(np.diff(T)) / np.sqrt(2) / np.mean(T)
    ext, big2, pm = [], [], []
    for i in range(len(mains) - 1):
        inside = [j for j in range(mains[i] + 1, mains[i + 1]) if s[j] < ms[i + 1]]
        ext.append(sum(1 for j in inside if disp[j] >= 0.15 * disp[mains[i]]))
        big2.append(sum(1 for j in inside if disp[j] >= 0.5 * disp[mains[i]]))
        pm.append(1.0 if ext[-1] > 0 else 0.0)
    sf = (e[mains] - s[mains])[:-1] / (np.diff(ms))
    stick = np.abs(v) <= THR
    creep = float(np.sqrt(np.mean(v[stick] ** 2))) if stick.any() else np.nan
    d2 = np.diff(f, 2)
    st2 = stick[1:-1]
    chat = float(np.sqrt(np.mean(d2[st2] ** 2)) / (np.mean(np.abs(f)) + 1e-30)) if st2.any() else np.nan
    sw = []
    for i in range(len(ms) - 1):
        seg = tau[ms[i]:ms[i + 1]]
        if len(seg) > 10: sw.append(seg.max() - seg.min())
    dm = disp[mains]
    return dict(nslip=len(s), jit=jit, extra=float(np.mean(ext)), big2=float(np.mean(big2)), pm=float(np.mean(pm)), slipfrac=float(np.mean(sf)), slipfrac_sd=float(np.std(sf)), creep=creep, chatter=chat,
                tau_sw=float(np.mean(sw)) if sw else np.nan, tau_mean=float(np.mean(tau)), T=float(np.mean(T)), main=len(mains), dsd=float(np.std(dm) / np.mean(dm)))


_REFL = {}


def ref_ltas(key):
    if key not in _REFL:
        _REFL[key] = R.ltas_third(R.sustain(R.ref_note(key))[0])
    return _REFL[key]


def audio_stats(key, y, tag):
    """HNR, jitter, shimmer (cycle-synchronous), mod shares, floors per octave and the mid-range (1-2 kHz) residual"""
    midi = R.NOTES[key][0]
    g = G.gates_of(y, midi, "rl_" + tag)
    seg, on = R.sustain(y)
    hd = R.harmonics(seg, R.hz(midi))
    fl = R.floor_oct(hd)
    # cycle stats on the note
    wav = os.path.join(OUT, "a_%s.wav" % tag)
    import scipy.io.wavfile as sw
    sw.write(wav, R.SR, y.astype(np.float32))
    nt = D.Note(wav, M.midi_hz(midi), tag)
    m = D.window_metrics(nt, (0.5, 1.4))
    os.remove(wav)
    gap = R.ltas_ranges(R.ltas_third(seg), ref_ltas(key))
    return dict(gap=gap, hnr=g["hnr"], mod4_7=g["mod4_7"], mod10=g["mod10"], jit=m["jit_pct"], shim=m["shim_db"], ppq5=m["ppq5"], floor=fl, tper=None, nf=m["nf_tot"])


def render_wav(key, seed, tag, knobs="", extra=(), vib=True, notefile=None):
    midi, st, nm = R.NOTES[key]
    wav = os.path.join(OUT, "%s_%s_s%d%s.wav" % (tag, nm, seed, "" if vib else "_nv"))
    if not os.path.exists(wav):
        env = dict(os.environ)
        env["ALTCELLO_KNOBS"] = knobs
        cmd = ["nice", os.path.join(ROOT, "altcello"), "render", notefile or nfile(key), wav, "--style", "expert", "--room", "off", "--seed", str(seed)] + (["--vib", "off"] if not vib else []) + [str(e) for e in extra]
        subprocess.run(cmd, capture_output=True, text=True, check=True, env=env)
    return M.load_audio(wav)


def measure_cfg(tag, knobs="", extra=(), notes=("D2", "G2c", "D3", "A3"), seeds=(0, 1, 2), vib=True, trace=True, notefiles=None):
    """per note: mean over seeds of audio and trace stats, and the time to periodic median"""
    out = {}
    for k in notes:
        rows = []
        for s in seeds:
            y = render_wav(k, s, tag, knobs, extra, vib, (notefiles or {}).get(k))
            a = audio_stats(k, y, "%s%s%d" % (tag, k, s))
            _, tp = L.t_periodic(y, R.hz(R.NOTES[k][0]), t_from=0.0, tag="rl_%s%s%d" % (tag, k, s))
            a["tper"] = tp
            if trace:
                a["tr"] = trace_stats(run_trace(k, s, tag, knobs, vib, extra), R.hz(R.NOTES[k][0]))
            rows.append(a)
        out[k] = rows
    return out


def mean(rows, key, sub=None):
    v = [(r[key] if sub is None else r[key][sub]) for r in rows]
    return float(np.nanmean(v))


def summary(res, ref=None):
    lines = []
    for k, rows in res.items():
        t = [r["tr"] for r in rows if "tr" in r]
        lines.append("%-4s HNR %5.1f jit_a %.2f%% shim %.2f dB mod4-7 %.2f mod10 %.2f floor %s tper %.2f" % (
            k, mean(rows, "hnr"), mean(rows, "jit"), mean(rows, "shim"), mean(rows, "mod4_7"), mean(rows, "mod10"),
            " ".join("%4.0f" % x for x in np.nanmean([r["floor"] for r in rows], axis=0)), float(np.median([r["tper"] for r in rows]))))
        if t:
            lines.append("     trace: jit %.2f%% extra/period %.2f (>=0.5 main: %.2f) slipsize sd %.2f slipfrac %.2f (sd %.2f) creep %.1e m/s chatter %.2e tau swing %.1f K mean %.1f K slips %d" % (
                np.nanmean([x["jit"] for x in t]), np.nanmean([x["extra"] for x in t]), np.nanmean([x["big2"] for x in t]), np.nanmean([x["dsd"] for x in t]), np.nanmean([x["slipfrac"] for x in t]), np.nanmean([x["slipfrac_sd"] for x in t]),
                np.nanmean([x["creep"] for x in t]), np.nanmean([x["chatter"] for x in t]), np.nanmean([x["tau_sw"] for x in t]), np.nanmean([x["tau_mean"] for x in t]), np.mean([x["nslip"] for x in t])))
    return "\n".join(lines)
