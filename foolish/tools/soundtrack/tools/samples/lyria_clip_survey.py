#!/usr/bin/env python3
"""lyria_clip_survey.py - one table row per Lyria solo clip, measured on the Demucs stems.

Usage: python3 -I lyria_clip_survey.py --clips DIR --prompts DIR --stems DIR --round3 DIR --tsv OUT.tsv --md OUT.md
  Clips are DIR/*/*_clip_solo_*.mp3; stems are STEMS/htdemucs/<clip name>/{drums,bass,other,vocals}.wav.
Per clip: round3 name (same byte size), asked notes (note tokens of the prompt), which stem is the pitched one
(other, or other+vocals summed when the vocals stem is within 12 dB of the other stem), played notes (stable pitch runs of
at least 0.5 s), real silences in the mix (stretches of at least 300 ms at least 40 dB below the clip peak),
PULSE (drums stem level re the mix, drums onsets, share of onsets within 40 ms of a regular grid at the best period and phase,
a steady pulse when at least 8 onsets and share >= 0.60, share of drums onsets within 40 ms of a pitched-stem onset),
bass stem level, POLYPHONY (100 ms frames of the pitched stem whose 186 ms analysis window holds one steady pitch (90 percent voiced, within 40 cents of the median, so a window straddling a note change is not counted): share of voiced frames with one distinct pitch, median purity),
and a verdict from the numbers.
Onsets: positive spectral flux (lyria_notes_extract.spectral_flux), peaks over 0.35 of the stem's 99th percentile flux that are
the largest within 0.2 s, only where the stem's 10 ms level is within 45 dB of the mix peak.
Removed-by-Demucs: the same pulse test on the mix and on the pitched stem: yes when the stem has no steady pulse, partly when
its grid share is under 0.8 of the mix's, no otherwise.
Deterministic, no randomness.
"""
import argparse, glob, hashlib, os, re, sys
import numpy as np
from scipy.io import wavfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lyria_notes_extract as le  # noqa: E402

TOK = re.compile(r"\b([A-G])(-sharp|-flat)?(\d)\b")
HOP = 0.010


def asked(prompt_path):
    if not os.path.exists(prompt_path):
        return []
    s = open(prompt_path).read()
    out = []
    for m in TOK.finditer(s):
        out.append(m.group(1) + {"-sharp": "#", "-flat": "b", None: ""}[m.group(2)] + m.group(3))
    return out


def read_stem(p):
    sr, x = wavfile.read(p)
    if x.dtype.kind == "i":
        x = x.astype(np.float64) / np.iinfo(x.dtype).max
    return x.astype(np.float64), sr


def rms_db(x):
    return 10 * np.log10(max(float(np.mean(x ** 2)), 1e-14))


def onsets(m, sr, floor_db):
    return le.flux_onsets(m, sr, floor_db)


def grid_share(on):
    """Best share of onsets within 40 ms of a regular grid (period from the inter-onset intervals, any phase)."""
    if len(on) < 2:
        return 0.0, 0.0
    on = np.array(on)
    ioi = np.diff(on)
    cands = sorted({round(float(v), 2) for v in ioi if 0.1 <= v <= 2.0})
    best, bp = 0.0, 0.0
    for P in cands:
        ph = np.mod(on, P)
        for p0 in ph:
            d = np.abs(((ph - p0 + P / 2) % P) - P / 2)
            sh = float(np.mean(d <= 0.040))
            if sh > best:
                best, bp = sh, P
    return best, bp


def silences(m, sr):
    db, h = le.level_db(m, sr)
    low = db < db.max() - 40.0
    n, i = 0, 0
    while i < len(low):
        if low[i]:
            j = i
            while j < len(low) and low[j]:
                j += 1
            if (j - i) * HOP >= 0.3:
                n += 1
            i = j
        else:
            i += 1
    return n


def polyphony(m, sr, db):
    f0, cen = le.yin_f0(m, sr)
    thr = db.max() - 40.0
    one = tot = 0
    purs = []
    N = le.NFFT
    for c in np.arange(0.5, len(m) / sr - 0.5, 0.1):
        fi = (cen >= (c - 0.05) * sr) & (cen < (c + 0.05) * sr)
        fv = f0[fi]
        fv = fv[fv > 0]
        a = int(c * sr) - N // 2
        wf = f0[(cen >= (c - 0.093) * sr) & (cen < (c + 0.093) * sr)]
        if len(fv) < 3 or len(wf) == 0 or np.mean(wf > 0) < 0.9 or \
                np.max(np.abs(1200 * np.log2(wf[wf > 0] / np.median(wf[wf > 0])))) > 40.0 or a < 0 or a + N > len(m) or db[min(int(c / HOP), len(db) - 1)] < thr:
            continue
        f = float(np.median(fv))
        pu, peaks = le.analyse_window(le.window_spectrum(m[a:a + N], f), sr, f)
        _, np_, _ = le.nonharmonic([(f, peaks)], sr)
        tot += 1
        one += (np_ == 1)
        purs.append(pu)
    return (one / tot if tot else 0.0), (float(np.median(purs)) if purs else 0.0), tot


def sha(p):
    return hashlib.sha1(open(p, "rb").read()).hexdigest()


def survey(clip, prompts, stems, r3sizes):
    name = os.path.splitext(os.path.basename(clip))[0]
    d = os.path.join(stems, "htdemucs", name)
    mix, sr = le.decode(clip)
    mm = mix.mean(axis=1)
    mixdb = rms_db(mm)
    mdb, _ = le.level_db(mm, sr)
    S = {k: read_stem(os.path.join(d, k + ".wav"))[0].mean(axis=1) for k in ("drums", "bass", "other", "vocals")}
    rel = {k: rms_db(v) - mixdb for k, v in S.items()}
    summed = rel["vocals"] >= rel["other"] - 12.0
    pit = S["other"] + S["vocals"] if summed else S["other"]
    pdb, _ = le.level_db(pit, sr)
    runs = [r for r in le.pitch_runs(pit, sr, pdb) if r[1] - r[0] >= 0.5]
    floor = float(mdb.max()) - 45.0
    don = onsets(S["drums"], sr, floor)
    pon = onsets(pit, sr, floor)
    mon = onsets(mm, sr, floor)
    gs_d, per_d = grid_share(don)
    gs_m, per_m = grid_share(mon)
    gs_p, per_p = grid_share(pon)
    coin = float(np.mean([min(abs(o - p) for p in pon) <= 0.04 for o in don])) if don and pon else 0.0
    one, pur, nfr = polyphony(pit, sr, pdb)
    sil = silences(mm, sr)
    steady = len(don) >= 8 and gs_d >= 0.60
    perc = rel["drums"] > -30.0 and coin < 0.60
    stem_pulse = len(pon) >= 8 and gs_p >= 0.60
    mix_pulse = len(mon) >= 8 and gs_m >= 0.60
    if perc or (steady and coin < 0.60):
        rem = "yes" if not stem_pulse else ("partly" if gs_p < 0.8 * gs_m else "no")
        verdict = "second instrument or percussion in the mix (removed by Demucs: %s); drums %.1f dB re mix, %d onsets, grid share %.2f, coincide with pitched onsets %.2f" % (
            rem, rel["drums"], len(don), gs_d, coin)
    elif nfr and one < 0.80:
        verdict = "several notes at once; one pitch in %.2f of %d steady frames" % (one, nfr)
    elif sil >= 3:
        verdict = "single voice, separated notes; %d silences" % sil
    else:
        verdict = "single voice, legato line; %d silences, one pitch in %.2f of %d steady frames" % (sil, one, nfr)
    r3 = r3sizes.get(sha(clip), "none")
    code = name.split("_")[0] + "_" + name.split("_")[1]
    ap = glob.glob(os.path.join(prompts, "*", name + ".txt"))
    return dict(clip=name, round3=r3, asked=" ".join(asked(ap[0]) if ap else []),
                pitched="other+vocals" if summed else "other", vocals_re_other="%.1f" % (rel["vocals"] - rel["other"]),
                played=" ".join(le.note_name(r[2]) for r in runs), runs05=len(runs), silences=sil,
                drums_db="%.1f" % rel["drums"], drum_onsets=len(don), drum_grid="%.2f" % gs_d, drum_period="%.2f" % per_d,
                steady_pulse="yes" if steady else "no", drum_pitched_coincide="%.2f" % coin,
                bass_db="%.1f" % rel["bass"], one_pitch_share="%.2f" % one, median_purity="%.3f" % pur, voiced_frames=nfr,
                mix_grid="%.2f" % gs_m, stem_grid="%.2f" % gs_p, verdict=verdict)


def main():
    ap = argparse.ArgumentParser()
    for a in ("clips", "prompts", "stems", "round3", "tsv", "md"):
        ap.add_argument("--" + a, required=True)
    a = ap.parse_args()
    r3 = {sha(p): os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(a.round3, "*.mp3"))}
    rows = [survey(c, a.prompts, a.stems, r3) for c in sorted(glob.glob(os.path.join(a.clips, "*", "*_clip_solo_*.mp3")))]
    cols = list(rows[0].keys())
    with open(a.tsv, "w") as f:
        f.write("\t".join(cols) + "\n")
        for r in rows:
            f.write("\t".join(str(r[c]) for c in cols) + "\n")
    L = ["# Round 3 solo clips, measured on the Demucs stems", "",
         "Method: see the docstring of `lyria_clip_survey.py`.", "",
         "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        L.append("| " + " | ".join(str(r[c]) for c in cols) + " |")
    open(a.md, "w").write("\n".join(L) + "\n")


if __name__ == "__main__":
    main()
