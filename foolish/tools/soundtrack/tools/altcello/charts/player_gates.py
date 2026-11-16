"""Gates of the expert player model: single notes of ALT-CELLO (EXPERT, NOVICE) against the Philharmonia notes of the same MIDI.

    python -I charts/player_gates.py G.txt ...   (see main) - library used by player_run.py and player.py
Definitions: diagnose.py (gates) and analysis/measure.py (analyze_note: attack_10_90_ms, vib_*, hnr_db). Windows are seconds after the
detected onset. Sustained-part windows are SUS = (0.5, 1.4) for both sides, so the ALT note (1.5 s, bow lifted at 1.5 s) and the longer
Philharmonia note are compared on the same stretch. Release: level of the 10 ms RMS at +50/+100/+200 ms after the end of the sound
(last frame within 3 dB of the median level over SUS), dB re that median (the lead's Philharmonia reference: -4 / -7 / -17).
"""
import os, sys, subprocess
import numpy as np
import scipy.io.wavfile as sw
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import diagnose as D  # noqa: E402
M = D.M
SR = M.SR
SUS = (0.5, 1.4)
PHIL_FULL = "/Users/alex/Dev/soundtrack-samples/philharmonia/wav_full/ph_cello_%d.wav"
TMP = os.path.join(ROOT, "out", "player")
KEYS = ["t_per", "att10_90", "att_tilt", "mod4_7", "mod10", "lev_sd", "vib_rate", "vib_depth", "vib_reg", "vib_depth_m", "vib_reg_m", "vib_onset", "hnr", "rel50", "rel100", "rel200", "cents"]
LABEL = {"t_per": "time to periodic s", "att10_90": "attack 10-90 ms", "att_tilt": "attack - sustain 2-6 kHz dB", "mod4_7": "4-7 Hz modulation share",
         "mod10": "above-10 Hz share", "lev_sd": "level sd dB", "vib_rate": "vibrato rate Hz (autocorr track, 0.35-1.45 s)", "vib_depth": "vibrato depth c peak (same; median of the Hilbert envelope)", "vib_reg": "vibrato regularity sd/mean (same)",
         "vib_depth_m": "vibrato depth c (measure.py, whole note)", "vib_reg_m": "vibrato regularity (measure.py, whole note)", "vib_onset": "vibrato onset s (measure.py)", "hnr": "HNR dB", "rel50": "release +50 ms dB", "rel100": "release +100 ms dB", "rel200": "release +200 ms dB",
         "cents": "pitch error c"}


def onset_s(y):
    env = M.rms_env(y)
    return int(np.argmax(env >= 0.02 * env.max())) * 0.01


def write_seg(y, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    sw.write(path, SR, y.astype(np.float32))


def gates_of(y, midi, tag, t_hint=None, dur=1.5):
    """y: mono float at 48 kHz holding ONE note with silence before it. returns dict of KEYS"""
    path = os.path.join(TMP, "seg_%s.wav" % tag)
    write_seg(y, path)
    fn = M.midi_hz(midi)
    nt = D.Note(path, fn, tag)
    g = D.gates(nt, SUS)
    on = onset_s(y)
    t0 = max(on - 0.02, 0.0)
    r, _ = M.analyze_note(y, SR, t0, dur, midi, 1e-6)
    vr, vd, vg, vm = vib_ac(y, midi, on)
    out = dict(t_per=g["t_per"], att10_90=r.get("attack_10_90_ms", np.nan), att_tilt=g["att_tilt"], mod4_7=g["mod4_7"], mod10=g["mod10"],
               lev_sd=g["lev_sd"], vib_rate=vr, vib_depth=vd, vib_reg=vg, vib_depth_m=r.get("vib_depth_c", np.nan), vib_reg_m=r.get("vib_regularity", np.nan),
               vib_onset=r.get("vib_onset_s", np.nan), hnr=g["hnr"], cents=r.get("cents_err", np.nan))
    env = 20 * np.log10(M.rms_env(y) + 1e-12)
    a, b = int((on + SUS[0]) * 100), int((on + SUS[1]) * 100)
    med = float(np.median(env[a:b]))
    j = np.where(env >= med - 3.0)[0]
    jend = int(j[j >= a][-1]) if len(j) else a
    for ms in (50, 100, 200):
        k = jend + ms // 10
        out["rel%d" % ms] = float(env[k] - med) if k < len(env) else np.nan
    out["_end_s"] = jend * 0.01 - on
    return out


def acpitch(y, fexp, t0, t1, hop=0.01):
    """normalised-autocorrelation pitch track (cents re fexp, within +-140 c), window max(40 ms, 5 periods): the vibrato measure of the
    player gates (the pyin curve of measure.py jumps in the unsettled start and inflates the depth)"""
    n = int(max(0.04, 5 / fexp) * SR)
    lo, hi = int(SR / (fexp * 1.08)), int(SR / (fexp / 1.08))
    ts = np.arange(t0, t1, hop)
    out = []
    for t in ts:
        a = int(t * SR)
        x = y[a:a + n + hi + 3]
        if len(x) < n + hi + 3:
            out.append(np.nan); continue
        u = x[:n]
        c = np.array([np.dot(u, x[l:l + n]) / np.sqrt(np.dot(u, u) * np.dot(x[l:l + n], x[l:l + n]) + 1e-30) for l in range(lo - 1, hi + 2)])
        k = int(np.argmax(c[1:-1])) + 1
        d = 0.5 * (c[k - 1] - c[k + 1]) / (c[k - 1] - 2 * c[k] + c[k + 1] + 1e-30)
        out.append(1200 * np.log2(SR / (lo - 1 + k + d) / fexp))
    return ts, np.array(out)


def vib_ac(y, midi, on, t0=0.35, t1=1.45):
    """rate (peak of the 3.5-9 Hz spectrum of the detrended track), depth (peak = sqrt 2 x sd of the 3.5-9 Hz band), regularity (sd/mean of the
    periods between upward zero crossings) over the sustained window t0..t1 after the onset"""
    import scipy.signal as ss
    ts, c = acpitch(y, M.midi_hz(midi), on + t0, on + t1)
    ok = np.isfinite(c)
    if ok.sum() < 60:
        return np.nan, np.nan, np.nan, np.nan
    c = np.interp(ts, ts[ok], c[ok])
    mean_c = float(np.median(c))
    cd = c - np.polyval(np.polyfit(ts, c, 1), ts)
    sp = np.abs(np.fft.rfft(cd * np.hanning(len(cd)), 4096))
    fq = np.fft.rfftfreq(4096, 0.01)
    m = (fq >= 3.5) & (fq <= 9)
    rate = float(fq[m][np.argmax(sp[m])])
    b, a = ss.butter(2, [3.5 / 50, 9 / 50], btype="band")
    cb = ss.filtfilt(b, a, cd)
    env = np.abs(ss.hilbert(cb))
    trim = 15
    core = env[trim:-trim] if len(env) > 2 * trim + 5 else env
    depth = float(np.median(core[core > 0.5 * core.max()]))  # measure.py's definition (median of the Hilbert envelope above half its maximum)
    up = np.where((cb[:-1] < 0) & (cb[1:] >= 0))[0]
    reg = np.nan
    if len(up) >= 4:
        tz = up + (-cb[up]) / (cb[up + 1] - cb[up])
        per = np.diff(tz) * 0.01
        per = per[(per > 0.5 / 9.0) & (per < 1.0 / 3.5)]
        if len(per) >= 3:
            reg = float(np.std(per) / np.mean(per))
    return rate, depth, reg, mean_c


def ref_gates(midi):
    y = M.load_audio(PHIL_FULL % midi)
    return gates_of(y, midi, "ref%d" % midi)


def alt_note_gates(y48, tstart, midi, tag, dur=1.5, pre=0.2, post=1.0):
    a = max(int((tstart - pre) * SR), 0)
    b = int((tstart + dur + post) * SR)
    seg = y48[a:b]
    return gates_of(seg, midi, tag, dur=dur)


def median_table(rows):
    return {k: float(np.nanmedian([r[k] for r in rows])) for k in KEYS}
