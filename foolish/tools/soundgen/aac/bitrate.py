"""Which AAC bitrate each sound needs to be transparent, by measurement (docs/AUDIO.md, "Shipped as AAC").
For every set of takes soundgen writes (a cue, and its tight and prelude sets), encode each WAV at each candidate
bitrate exactly as the build does (ffmpeg's native aac, mono), decode it again, and hold the decoded set against
the WAVs: (1) the 1/12-octave spectrum, the median over the takes within 0.5 dB in every band within 30 dB of
the takes' loudest, 30 dB being well under what masks codec noise there (and for the 48 kHz sets 15-20 kHz too, which the thumb slide fills); (2) ../throwin/an/measure.py (both
resampled to 48 kHz, which it measures at): every measure's median moved by no more than a quarter of the WAVs' own
spread across their takes, or by its floor (FLOOR) for a set too uniform to have a spread. The lowest bitrate that
passes is the answer; a set no candidate passes says which measures held it back.
Usage (from tools/soundgen, after `make wavs`): python3 -I aac/bitrate.py OUTDIR [SET ...]
  SET is a file stem without its take number (deal, lift_tight, sweep_strokes_s3); none = every set."""
import sys, os, glob, re, subprocess, wave, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); SG = os.path.dirname(HERE)
OUT = sys.argv[1]; os.makedirs(os.path.join(OUT, "plots"), exist_ok=True)
# A snapshot of the WAVs, so a `make sounds` meanwhile cannot pull them out from under the run.
WAVS = os.path.join(OUT, "wav")
if not os.path.isdir(WAVS):
    import shutil; shutil.copytree(os.path.join(SG, "build", "wav"), WAVS)
CANDIDATES = [48, 64, 96, 128, 160, 192]
sets = {}
for p in sorted(glob.glob(os.path.join(WAVS, "*.wav"))):
    sets.setdefault(re.sub(r"_\d+\.wav$", "", os.path.basename(p)), []).append(p)
only = sys.argv[2:]
def sh(*a): subprocess.run(a, check=True, capture_output=True)
def rate(p):
    with wave.open(p) as w: return w.getframerate()
def to48(src, dst): sh("ffmpeg", "-y", "-loglevel", "error", "-i", src, "-ar", "48000", "-c:a", "pcm_s16le", dst)
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768, w.getframerate()
def oct12(x, sr):
    top = min(20000, sr / 2 * 0.97); cs = np.geomspace(150, top, int(12 * np.log2(top / 150)))
    X = np.abs(np.fft.rfft(x)) ** 2; f = np.fft.rfftfreq(len(x), 1 / sr)
    return np.array([10 * np.log10(X[(f >= c * 2 ** (-1 / 24)) & (f < c * 2 ** (1 / 24))].sum() + 1e-20) for c in cs])
def spec_diff(a, b):
    x, sr = load(a); y, _ = load(b); n = min(len(x), len(y)); sx, sy = oct12(x[:n], sr), oct12(y[:n], sr)
    return sy - sx, sx > sx.max() - 30
# How far a measure may move in a set with no spread across its takes (one take, or a placeholder).
FLOOR = {"dB": 0.5, "ms": 2.0, "Hz": 0.03, "clicks": 1.0, "kurtosis": 0.1, "flatness": 0.02}
def floor_of(k, v):
    if k.startswith("kurtosis"): return FLOOR["kurtosis"] * abs(v)
    if "Hz" in k: return FLOOR["Hz"] * abs(v)
    if k.startswith("flatness"): return FLOOR["flatness"]
    if k == "clicks": return FLOOR["clicks"]
    if k.endswith("ms"): return FLOOR["ms"]
    return FLOOR["dB"]
def measure(name, a, b):
    o = subprocess.run([sys.executable, "-I", os.path.join(SG, "throwin/an/measure.py"), name, a, b], capture_output=True, text=True, cwd=OUT).stdout
    rows = {}
    for l in o.split("\n"):
        m = re.match(r"(.{24}) +(-?[\d.]+|nan) +(-?[\d.]+|nan) +(-?[\d.]+|nan) +(-?[\d.]+|nan)", l)
        if m: rows[m.group(1).strip()] = tuple(float(m.group(i)) for i in range(2, 6))
    spec = [float(v) for v in re.search(r"B-A +(.*)\n", o).group(1).split()]
    return rows, spec
for stem, files in sets.items():
    if only and stem not in only: continue
    sr = rate(files[0]); d = os.path.join(OUT, stem); os.makedirs(d, exist_ok=True)
    for p in files: to48(p, os.path.join(d, "a_" + os.path.basename(p)))
    verdict = None
    for br in CANDIDATES:
        diffs, keeps = [], []
        for p in files:
            m4a = os.path.join(d, f"{br}.m4a"); dec = os.path.join(d, f"{br}_dec.wav")
            # exactly the build's encode (c/m4a.c m4a_encode)
            sh("ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-i", p, "-c:a", "aac", "-aac_coder", "fast", "-b:a", f"{br}k", "-ac", "1",
               "-movie_timescale", str(sr), "-map_metadata", "-1", "-fflags", "+bitexact", "-flags:a", "+bitexact", m4a)
            sh("ffmpeg", "-y", "-loglevel", "error", "-i", m4a, "-c:a", "pcm_s16le", dec)
            to48(dec, os.path.join(d, f"b{br}_" + os.path.basename(p)))
            d_, k_ = spec_diff(p, dec); diffs.append(d_); keeps.append(k_)
        rows, spec = measure(f"{stem}_{br}", os.path.join(d, "a_*.wav"), os.path.join(d, f"b{br}_*.wav"))
        bad = []
        ws = float(np.abs(np.median(diffs, 0))[np.any(keeps, 0)].max())
        for k, (ma, lo, hi, mb) in rows.items():
            if k == "peak dBFS" or np.isnan(ma) or np.isnan(mb): continue
            # a measure of a band the file has no room for (above its Nyquist) measures the resampler's noise
            if sr < 24000 and ("4-12k" in k or k.startswith("click") or k.startswith("micro") or k.startswith("rough")): continue
            tol = max(0.25 * (hi - lo), floor_of(k, ma))
            if abs(mb - ma) > tol: bad.append(f"{k} {ma:.2f}->{mb:.2f}")
        ok = not bad and ws <= 0.5
        print(f"{stem} {sr} {br}k: spectrum worst {ws:.2f} dB, {len(bad)} measures off{': ' + '; '.join(bad[:4]) if bad else ''}{'  PASS' if ok else ''}", flush=True)
        if ok: verdict = br; break
    print(f"== {stem}: {verdict or 'none of ' + str(CANDIDATES)}k", flush=True)
