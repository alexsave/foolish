#!/usr/bin/env python3
"""vocalset_extract.py - cut sustained sung notes out of VocalSet into a sampler instrument.

VocalSet (Wilkins, Seetharaman, Wahl, Pardo, ISMIR 2018), https://zenodo.org/records/1193957, CC BY 4.0

What it does
  For each male singer and each selected VocalSet file (long_tones forte/pp/straight with vowels a o u,
  scales slow_forte/slow_piano with vowels a o) it
    1. tracks f0 (YIN, 10 ms hop, 60 to 600 Hz) and frame RMS,
    2. finds note segments: stretches where f0 stays within +-60 cents of the segment's running median
       (3 consecutive bad frames end a segment) and the frame level stays above -35 dB relative to the
       loudest frame of the file; minimum length 1.2 s (long_tones) or 0.45 s (scales),
    3. measures per segment: start, end, median f0, nearest MIDI note, cents offset, f0 sd in cents,
       vibrato rate and depth (4 to 8 Hz, depth = peak deviation in cents, only if periodic), RMS,
    4. skips segments whose cents offset from the nearest note is over 45, or whose f0 sd is over 60 cents,
    5. writes each kept segment as 48 kHz 24-bit mono wav, starting 30 ms before the onset (but not before
       the end of the previous segment), ending at the segment end with a 40 ms fade-out (and a 5 ms
       fade-in), resampled so that the median f0 lands exactly on the equal-tempered pitch (A4 = 440),
       gained so the segment RMS hits the technique's target dBFS (forte, straight -20; pp,
       slow_piano -29; slow_forte -22) while the peak never passes -1 dBFS (gain reduced and flagged),
    6. writes the sampler manifests choir_<singer>_ah.txt (vowel a) and choir_<singer>_oh.txt (vowels o, u),
       a TSV with one row per written sample and a Markdown coverage report.
  Manifest line: `root_midi layer rr path`. Layer 0 = pp long tones plus slow_piano scale notes, layer 1 =
  forte and straight long tones plus slow_forte scale notes. rr counts from 1 within (root, layer).
  A scale-note segment is only used for a (root, layer) that has no long-tone segment.

Inputs
  --vocalset DIR   directory holding <singer>/<exercise>/<technique>/<file>.wav (VocalSet FULL/)
Outputs
  --out DIR        wavs go to DIR/<singer>/<singer>_<exercise>_<technique>_<vowel>_m<midi>_<n>.wav
  --manifests DIR  choir_<singer>_ah.txt and choir_<singer>_oh.txt
  --tsv FILE       one row per written sample
  --md FILE        coverage report

Example (whole set)
  nice -n 10 python3 -I vocalset_extract.py --vocalset /path/vocalset/FULL --out /path/vocalset_choir \\
      --manifests /path/samples/manifests --tsv vocalset_choir.tsv --md vocalset_choir.md
Example (one file, used by the test)
  python3 -I vocalset_extract.py --file x.wav --singer test --exercise long_tones --technique forte \\
      --vowel a --out outdir --tsv rows.tsv

Failure modes
  Missing input directory or file: prints an error and exits 2.
  Input not 16-bit or 24-bit PCM wav: scipy raises; the file is reported and the run stops.
  Octave errors from YIN end a segment (f0 jump over 60 cents) and are not corrected.
  A singer or vowel with no accepted segment simply gets no manifest lines (an empty manifest is still written).
  Deterministic: no randomness, same input gives byte-identical output.
"""
import argparse
import os
import struct
import sys
from fractions import Fraction

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

ATTRIBUTION = ("VocalSet (Wilkins, Seetharaman, Wahl, Pardo, ISMIR 2018), "
               "https://zenodo.org/records/1193957, CC BY 4.0")

OUT_SR = 48000
HOP_S = 0.010
WIN = 2048
FMIN, FMAX = 60.0, 600.0
YIN_THR = 0.15
LEVEL_DB = -35.0
DEV_CENTS = 60.0
BAD_FRAMES = 3
PRE_ROLL_S = 0.030
FADE_OUT_S = 0.040
FADE_IN_S = 0.005
MAX_OFFSET_CENTS = 45.0
MAX_SD_CENTS = 60.0
PEAK_LIMIT_DB = -1.0
MIN_LEN = {"long_tones": 1.2, "scales": 0.45}

# technique -> (target rms dBFS, layer)
TECH = {"forte": (-20.0, 1), "straight": (-20.0, 1), "pp": (-29.0, 0),
        "slow_forte": (-22.0, 1), "slow_piano": (-29.0, 0)}
# (exercise, technique, vowels)
SOURCES = [("long_tones", "forte", ("a", "o", "u")),
           ("long_tones", "pp", ("a", "o", "u")),
           ("long_tones", "straight", ("a", "o", "u")),
           ("scales", "slow_forte", ("a", "o")),
           ("scales", "slow_piano", ("a", "o"))]
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(m):
    return "%s%d" % (NOTE_NAMES[m % 12], m // 12 - 1)


def midi_to_hz(m):
    return 440.0 * 2.0 ** ((m - 69) / 12.0)


# ---------------------------------------------------------------- wav io
def read_wav(path):
    """Returns (float64 mono array in [-1,1], sample rate). Handles 16 and 24 bit PCM."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == b"RIFF":
        pos, fmt, pcm = 12, None, None
        while pos + 8 <= len(data):
            cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
            body = data[pos + 8:pos + 8 + size]
            if cid == b"fmt ":
                fmt = struct.unpack("<HHIIHH", body[:16])
            elif cid == b"data":
                pcm = body
            pos += 8 + size + (size & 1)
        tag, ch, sr, _, _, bits = fmt
        if tag == 1 and bits == 24:
            raw = np.frombuffer(pcm[:len(pcm) // 3 * 3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
            v = raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)
            v = np.where(v >= 1 << 23, v - (1 << 24), v)
            x = v.astype(np.float64) / (1 << 23)
            return (x.reshape(-1, ch).mean(axis=1) if ch > 1 else x), sr
    sr, x = wavfile.read(path)
    if x.dtype == np.int16:
        x = x.astype(np.float64) / 32768.0
    elif x.dtype == np.int32:
        x = x.astype(np.float64) / 2147483648.0
    else:
        x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    return x, sr


def write_wav24(path, x, sr):
    v = np.clip(np.round(x * (2 ** 23 - 1)), -(2 ** 23), 2 ** 23 - 1).astype(np.int32)
    v = np.where(v < 0, v + (1 << 24), v).astype(np.uint32)
    b = np.stack([v & 255, (v >> 8) & 255, (v >> 16) & 255], axis=1).astype(np.uint8).tobytes()
    hdr = b"RIFF" + struct.pack("<I", 36 + len(b)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, sr, sr * 3, 3, 24)
    hdr += b"data" + struct.pack("<I", len(b))
    with open(path, "wb") as f:
        f.write(hdr + b)


# ---------------------------------------------------------------- analysis
def track(x, sr):
    """Returns (f0 Hz per frame (0 = unvoiced), rms dBFS per frame, frame center sample index)."""
    hop = int(round(HOP_S * sr))
    tau_min = int(sr / FMAX)
    tau_max = int(sr / FMIN) + 1
    need = WIN + tau_max
    nfr = max(0, (len(x) - need) // hop + 1)
    if nfr == 0:
        return np.zeros(0), np.zeros(0), np.zeros(0, dtype=int)
    nfft = 4096
    f0 = np.zeros(nfr)
    rmsdb = np.zeros(nfr)
    centers = np.arange(nfr) * hop + WIN // 2
    idx = np.arange(nfr)[:, None] * hop + np.arange(need)[None, :]
    frames = x[idx]
    a = frames[:, :WIN]
    A = np.fft.rfft(a, nfft, axis=1)
    B = np.fft.rfft(frames, nfft, axis=1)
    cross = np.fft.irfft(np.conj(A) * B, nfft, axis=1)[:, :tau_max + 1]
    e1 = (a ** 2).sum(axis=1)
    cs = np.concatenate([np.zeros((nfr, 1)), np.cumsum(frames ** 2, axis=1)], axis=1)
    taus = np.arange(tau_max + 1)
    e2 = cs[:, WIN:WIN + tau_max + 1] - cs[:, taus]
    d = e1[:, None] + e2 - 2.0 * cross
    d[:, 0] = 0.0
    csum = np.cumsum(d[:, 1:], axis=1)
    cmnd = np.ones_like(d)
    cmnd[:, 1:] = d[:, 1:] * taus[1:][None, :] / np.maximum(csum, 1e-12)
    for i in range(nfr):
        rmsdb[i] = 10.0 * np.log10(max(e1[i] / WIN, 1e-20))
        c = cmnd[i]
        t = tau_min
        below = np.nonzero(c[tau_min:tau_max] < YIN_THR)[0]
        if len(below) == 0:
            continue
        t = tau_min + below[0]
        while t + 1 < tau_max and c[t + 1] < c[t]:
            t += 1
        if t <= 1 or t + 1 > tau_max:
            continue
        y0, y1, y2 = c[t - 1], c[t], c[t + 1]
        den = y0 - 2 * y1 + y2
        off = 0.5 * (y0 - y2) / den if abs(den) > 1e-12 else 0.0
        f0[i] = sr / (t + off)
    return f0, rmsdb, centers


def vibrato(cents, hop_s):
    """cents: f0 track in cents (frame rate 1/hop_s). Returns (rate Hz, depth cents) or (0, 0) if not periodic."""
    n = len(cents)
    if n * hop_s < 0.8:
        return 0.0, 0.0
    t = np.arange(n)
    c = cents - np.polyval(np.polyfit(t, cents, 1), t)
    w = np.hanning(n)
    nf = 4096
    X = np.abs(np.fft.rfft(c * w, nf)) * 2.0 / w.sum()
    fr = np.fft.rfftfreq(nf, hop_s)
    band = (fr >= 4.0) & (fr <= 8.0)
    ref = (fr >= 1.0) & (fr <= 15.0) & ~band
    k = np.argmax(np.where(band, X, 0.0))
    amp = X[k]
    floor = np.sqrt(np.mean(X[ref] ** 2)) if ref.any() else 1e9
    if amp >= 8.0 and amp >= 3.0 * floor:
        return float(fr[k]), float(amp)
    return 0.0, 0.0


def find_segments(x, sr, min_len):
    """Returns list of dicts: start, end (seconds), f0 (median Hz), sd (cents), vib_rate, vib_depth, rms_db."""
    f0, rmsdb, centers = track(x, sr)
    if len(f0) == 0:
        return []
    thr = rmsdb.max() + LEVEL_DB
    ok = (f0 > 0) & (rmsdb > thr)
    hop = int(round(HOP_S * sr))
    segs = []
    cur = []          # frame indices in current segment
    bad = []          # pending bad frames (with their ok flag)

    def close():
        if cur:
            segs.append(list(cur))

    i = 0
    n = len(f0)
    while i < n:
        if ok[i]:
            if not cur:
                cur.append(i)
                bad = []
            else:
                med = np.median(f0[cur])
                if abs(1200.0 * np.log2(f0[i] / med)) <= DEV_CENTS:
                    cur.append(i)
                    bad = []
                else:
                    bad.append(i)
        else:
            bad.append(i)
        if len(bad) >= BAD_FRAMES:
            close()
            seed = [b for b in bad if ok[b]]
            cur = []
            bad = []
            if seed:
                cur = [seed[-1]] if len(seed) == 1 else []
                # restart from the deviant frames only if they agree with each other
                if len(seed) > 1:
                    m = np.median(f0[seed])
                    cur = [s for s in seed if abs(1200.0 * np.log2(f0[s] / m)) <= DEV_CENTS]
        i += 1
    close()
    out = []
    prev_end = 0.0
    for fr in segs:
        s = (centers[fr[0]] - hop / 2.0) / sr
        e = (centers[fr[-1]] + hop / 2.0) / sr
        if e - s < min_len:
            continue
        fv = f0[fr]
        med = float(np.median(fv))
        cents = 1200.0 * np.log2(fv / med)
        vr, vd = vibrato(cents, HOP_S)
        a0, a1 = int(max(s, 0) * sr), int(e * sr)
        seg = x[a0:a1]
        rms = float(np.sqrt(np.mean(seg ** 2)))
        out.append(dict(start=max(s, 0.0), end=min(e, len(x) / sr), f0=med, sd=float(np.std(cents)),
                        vib_rate=vr, vib_depth=vd, rms_db=20.0 * np.log10(max(rms, 1e-12)),
                        prev_end=prev_end))
        prev_end = out[-1]["end"]
    return out


def classify(seg):
    m = int(round(69 + 12 * np.log2(seg["f0"] / 440.0)))
    seg["midi"] = m
    seg["offset_cents"] = 1200.0 * np.log2(seg["f0"] / midi_to_hz(m))
    return seg


def render(x, sr, seg, target_db):
    """Cut, fade, resample to 48 kHz with the pitch correction, gain. Returns (audio, info)."""
    a0 = int(round(max(seg["start"] - PRE_ROLL_S, seg["prev_end"]) * sr))
    a1 = int(round(seg["end"] * sr))
    y = x[a0:a1].copy()
    rms_src = np.sqrt(np.mean(x[int(seg["start"] * sr):a1] ** 2))
    nfo = int(FADE_OUT_S * sr)
    nfi = int(FADE_IN_S * sr)
    y[len(y) - nfo:] *= np.linspace(1.0, 0.0, nfo)
    y[:nfi] *= np.linspace(0.0, 1.0, nfi)
    ratio = (OUT_SR / sr) * seg["f0"] / midi_to_hz(seg["midi"])
    fr = Fraction(ratio).limit_denominator(600)
    z = resample_poly(y, fr.numerator, fr.denominator)
    gain = 10.0 ** (target_db / 20.0) / max(rms_src, 1e-12)
    # the rms reference is the resampled audio over the segment proper (resampling changes the rms by under 1e-4 dB)
    peak = np.max(np.abs(z)) * gain
    limited = False
    if peak > 10.0 ** (PEAK_LIMIT_DB / 20.0):
        gain *= 10.0 ** (PEAK_LIMIT_DB / 20.0) / peak
        limited = True
    z = z * gain
    info = dict(correction_cents=-seg["offset_cents"], gain_db=20.0 * np.log10(gain),
                limited=limited, out_rms_db=20.0 * np.log10(max(rms_src * gain, 1e-12)),
                peak_db=20.0 * np.log10(max(np.max(np.abs(z)), 1e-12)))
    return z, info


def process_file(x, sr, singer, exercise, technique, vowel, relpath, outdir, counters):
    """Analyse one file and write its wavs. Returns (rows, skipped dict)."""
    target_db, layer = TECH[technique]
    segs = [classify(s) for s in find_segments(x, sr, MIN_LEN[exercise])]
    rows = []
    skipped = {"offset": 0, "sd": 0}
    for s in segs:
        if abs(s["offset_cents"]) > MAX_OFFSET_CENTS:
            skipped["offset"] += 1
            continue
        if s["sd"] > MAX_SD_CENTS:
            skipped["sd"] += 1
            continue
        key = (singer, exercise, technique, vowel, s["midi"])
        counters[key] = counters.get(key, 0) + 1
        name = "%s_%s_%s_%s_m%d_%d.wav" % (singer, exercise, technique, vowel, s["midi"], counters[key])
        z, info = render(x, sr, s, target_db)
        os.makedirs(os.path.join(outdir, singer), exist_ok=True)
        write_wav24(os.path.join(outdir, singer, name), z, OUT_SR)
        rows.append(dict(singer=singer, source=relpath, start=s["start"], end=s["end"], midi=s["midi"],
                         note=note_name(s["midi"]), correction=info["correction_cents"], sd=s["sd"],
                         vib_rate=s["vib_rate"], vib_depth=s["vib_depth"], duration=len(z) / OUT_SR,
                         layer=layer, manifest="-", exercise=exercise, technique=technique, vowel=vowel,
                         wav=name, gain_db=info["gain_db"], limited=int(info["limited"]),
                         rms_db=info["out_rms_db"], peak_db=info["peak_db"]))
    return rows, skipped


# ---------------------------------------------------------------- reports
TSV_COLS = ["singer", "source", "start", "end", "midi", "note", "correction_cents", "f0_sd_cents",
            "vib_rate_hz", "vib_depth_cents", "duration_s", "layer", "manifest", "wav", "gain_db",
            "peak_limited", "rms_dbfs", "peak_dbfs"]


def write_tsv(path, rows):
    with open(path, "w") as f:
        f.write("\t".join(TSV_COLS) + "\n")
        for r in rows:
            f.write("\t".join([r["singer"], r["source"], "%.3f" % r["start"], "%.3f" % r["end"],
                               str(r["midi"]), r["note"], "%.1f" % r["correction"], "%.1f" % r["sd"],
                               "%.2f" % r["vib_rate"], "%.1f" % r["vib_depth"], "%.3f" % r["duration"],
                               str(r["layer"]), r["manifest"], r["wav"], "%.2f" % r["gain_db"],
                               str(r["limited"]), "%.2f" % r["rms_db"], "%.2f" % r["peak_db"]]) + "\n")


def build_manifest(rows, singer, vowels, prefix):
    """Selects rows for a manifest, sets r['manifest'], returns (lines, chosen rows)."""
    cand = [r for r in rows if r["singer"] == singer and r["vowel"] in vowels]
    chosen = []
    for layer in (0, 1):
        for root in sorted({r["midi"] for r in cand if r["layer"] == layer}):
            group = [r for r in cand if r["layer"] == layer and r["midi"] == root]
            longs = [r for r in group if r["exercise"] == "long_tones"]
            use = longs if longs else group
            use = sorted(use, key=lambda r: (r["exercise"], r["technique"], r["vowel"], r["wav"]))
            chosen.extend(use)
    lines = []
    for layer in (0, 1):
        for root in sorted({r["midi"] for r in chosen if r["layer"] == layer}):
            for rr, r in enumerate([r for r in chosen if r["layer"] == layer and r["midi"] == root], 1):
                lines.append("%d %d %d %s/%s/%s" % (root, layer, rr, prefix, singer, r["wav"]))
    return lines, chosen


def note_list(ms):
    return " ".join(note_name(m) for m in sorted(set(ms))) if ms else "none"


def write_md(path, singers, rows, skipped, found, tag_of):
    L = []
    L.append("# VocalSet choir sampler set")
    L.append("")
    L.append(ATTRIBUTION + ".")
    L.append("Notes listed are the ones in the manifests (long tones first, scale notes only where a long tone is missing).")
    L.append("Note names use C4 = MIDI 60, A4 = 440 Hz.")
    L.append("Layer 0 is pp plus slow_piano, layer 1 is forte plus straight plus slow_forte.")
    L.append("")
    for s in singers:
        used = [r for r in rows if r["singer"] == s and r["manifest"] != "-"]
        L.append("## %s" % s)
        L.append("")
        L.append("Segments found: %d, written: %d, in manifests: %d, skipped: %d (cents offset over 45: %d, f0 sd over 60: %d)."
                 % (found[s], len([r for r in rows if r["singer"] == s]), len(used),
                    skipped[s]["offset"] + skipped[s]["sd"], skipped[s]["offset"], skipped[s]["sd"]))
        L.append("")
        for v in ("a", "o", "u"):
            for layer in (0, 1):
                ms = [r["midi"] for r in used if r["vowel"] == v and r["layer"] == layer]
                if ms:
                    L.append("- Vowel %s, layer %d (%d samples): %s (lowest %s, highest %s)"
                             % (v, layer, len(ms), note_list(ms), note_name(min(ms)), note_name(max(ms))))
                else:
                    L.append("- Vowel %s, layer %d: none" % (v, layer))
        for grp, vs in (("ah", ("a",)), ("oh", ("o", "u"))):
            ms = [r["midi"] for r in used if r["vowel"] in vs]
            L.append("- Manifest choir_%s_%s.txt: %d lines, lowest %s, highest %s"
                     % (s, grp, len(ms), note_name(min(ms)) if ms else "none", note_name(max(ms)) if ms else "none"))
        L.append("")
    L.append("## Pitch coverage, vowel a, layer 1")
    L.append("")
    L.append("| note | singers with it (of %d) |" % len(singers))
    L.append("|---|---|")
    for m in range(40, 70):
        have = {r["singer"] for r in rows if r["manifest"] != "-" and r["vowel"] == "a"
                and r["layer"] == 1 and r["midi"] == m}
        L.append("| %s (%d) | %d |" % (note_name(m), m, len(have)))
    L.append("")
    with open(path, "w") as f:
        f.write("\n".join(L))


# ---------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser(description="Extract sustained notes from VocalSet into a sampler set.")
    ap.add_argument("--vocalset")
    ap.add_argument("--out", required=True)
    ap.add_argument("--manifests")
    ap.add_argument("--tsv", required=True)
    ap.add_argument("--md")
    ap.add_argument("--singers", help="comma separated; default every male* directory")
    ap.add_argument("--manifest-prefix", default="/SAMPLES/soundtrack/samples/vocalset_choir")
    ap.add_argument("--file")
    ap.add_argument("--singer")
    ap.add_argument("--exercise")
    ap.add_argument("--technique")
    ap.add_argument("--vowel")
    a = ap.parse_args(argv)
    counters = {}
    rows, skipped, found = [], {}, {}
    if a.file:
        if not os.path.isfile(a.file):
            print("error: no such file %s" % a.file, file=sys.stderr)
            return 2
        x, sr = read_wav(a.file)
        r, sk = process_file(x, sr, a.singer, a.exercise, a.technique, a.vowel, os.path.basename(a.file),
                             a.out, counters)
        write_tsv(a.tsv, r)
        return 0
    if not a.vocalset or not os.path.isdir(a.vocalset):
        print("error: --vocalset directory missing", file=sys.stderr)
        return 2
    singers = a.singers.split(",") if a.singers else sorted(
        [d for d in os.listdir(a.vocalset) if d.startswith("male")], key=lambda d: int(d[4:]))
    for s in singers:
        skipped[s] = {"offset": 0, "sd": 0}
        found[s] = 0
        for exercise, technique, vowels in SOURCES:
            d = os.path.join(a.vocalset, s, exercise, technique)
            if not os.path.isdir(d):
                print("warning: missing %s" % d, file=sys.stderr)
                continue
            for fn in sorted(os.listdir(d)):
                if not fn.endswith(".wav"):
                    continue
                vowel = fn[:-4].rsplit("_", 1)[1]
                if vowel not in vowels:
                    continue
                x, sr = read_wav(os.path.join(d, fn))
                r, sk = process_file(x, sr, s, exercise, technique, vowel,
                                     "%s/%s/%s" % (exercise, technique, fn), a.out, counters)
                rows.extend(r)
                skipped[s]["offset"] += sk["offset"]
                skipped[s]["sd"] += sk["sd"]
                found[s] += len(r) + sk["offset"] + sk["sd"]
        print("%s: %d samples" % (s, len([r for r in rows if r["singer"] == s])), file=sys.stderr)
    if a.manifests:
        os.makedirs(a.manifests, exist_ok=True)
        for s in singers:
            for grp, vs in (("ah", ("a",)), ("oh", ("o", "u"))):
                lines, chosen = build_manifest(rows, s, vs, a.manifest_prefix)
                mname = "choir_%s_%s.txt" % (s, grp)
                for r in chosen:
                    r["manifest"] = mname
                with open(os.path.join(a.manifests, mname), "w") as f:
                    f.write("\n".join(lines) + ("\n" if lines else ""))
    write_tsv(a.tsv, rows)
    if a.md:
        write_md(a.md, singers, rows, skipped, found, None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
