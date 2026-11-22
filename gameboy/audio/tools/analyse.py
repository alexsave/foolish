#!/usr/bin/env python3
# analyse.py - measure the website's real takes (soundgen's WAVs), the numbers
# DESIGN.md's tables quote and fit.py fits to. A non-production tool.
#
#   python3 -I tools/analyse.py WAVDIR CUE_TAKES_C > build/analysis.tsv
#
# WAVDIR is `soundgen write`'s output, CUE_TAKES_C is `soundgen ctable`'s
# (for the anchors). Prints one row a take, tab separated, plain text.
# No randomness: every number is a pure function of the files.
import sys, os, re, wave
import numpy as np


def read_wav(path):
    with wave.open(path, "rb") as w:
        sr, n, ch, sw = w.getframerate(), w.getnframes(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    assert sw == 2, path
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return sr, x


def anchors(ctable):
    out = {}
    pat = re.compile(r'\{\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*"([^"]+)"')
    for line in open(ctable):
        m = pat.search(line)
        if m:
            out[m.group(8)] = int(m.group(3)) / 1000.0  # ms
    return out


def rms_env(x, sr, win_ms):
    n = max(1, int(sr * win_ms / 1000))
    k = len(x) // n
    if k == 0:
        return np.array([np.sqrt(np.mean(x * x))])
    return np.sqrt(np.mean(x[: k * n].reshape(k, n) ** 2, axis=1) + 1e-20)


def db(v):
    return 20 * np.log10(np.maximum(v, 1e-10))


def measure(sr, x):
    peak = np.max(np.abs(x))
    e5 = rms_env(x, sr, 5)
    e50 = rms_env(x, sr, 50)
    loud = db(e5).max()
    above = np.nonzero(db(e5) > loud - 30)[0]
    dur30 = (above[-1] - above[0] + 1) * 5 if len(above) else 0
    # spectrum of the whole take, energy weighted
    nfft = 1 << int(np.ceil(np.log2(len(x))))
    X = np.abs(np.fft.rfft(x, nfft)) ** 2
    f = np.fft.rfftfreq(nfft, 1 / sr)
    tot = X.sum() + 1e-30
    frac = lambda lo: X[f >= lo].sum() / tot
    centroid = (X * f).sum() / tot
    # onsets: the 5 ms envelope jumping 12 dB over the frame before
    d = db(e5)
    onsets = int(np.sum((d[1:] - d[:-1] > 12) & (d[1:] > loud - 30)))
    # rise: from -20 dB under the peak frame to the peak, ms
    pk = int(np.argmax(e5))
    i = pk
    while i > 0 and d[i] > d[pk] - 20:
        i -= 1
    rise = (pk - i) * 5
    # fall: peak to -20 dB, ms
    j = pk
    while j < len(d) - 1 and d[j] > d[pk] - 20:
        j += 1
    fall = (j - pk) * 5
    return dict(len_ms=len(x) / sr * 1000, peak_db=db(peak), rms50_db=db(e50).max(),
                dur30_ms=dur30, rise_ms=rise, fall_ms=fall, onsets=onsets,
                peak_ms=pk * 5 + 2.5, centroid=centroid,
                above4k=frac(4000), above8k=frac(8000), above16k=frac(16000), below500=1 - frac(500))


def main():
    wavdir, ctable = sys.argv[1], sys.argv[2]
    anc = anchors(ctable)
    cols = ["take", "rate", "len_ms", "anchor_ms", "peak_ms", "peak_db", "rms50_db", "dur30_ms", "rise_ms",
            "fall_ms", "onsets", "centroid", "below500", "above4k", "above8k", "above16k"]
    print("\t".join(cols))
    for name in sorted(os.listdir(wavdir)):
        if not name.endswith(".wav"):
            continue
        stem = name[:-4]
        sr, x = read_wav(os.path.join(wavdir, name))
        m = measure(sr, x)
        row = [stem, str(sr), "%.0f" % m["len_ms"], "%.1f" % anc.get(stem, float("nan")), "%.1f" % m["peak_ms"],
               "%.1f" % m["peak_db"], "%.1f" % m["rms50_db"], "%d" % m["dur30_ms"], "%d" % m["rise_ms"],
               "%d" % m["fall_ms"], "%d" % m["onsets"], "%.0f" % m["centroid"], "%.2f" % m["below500"],
               "%.2f" % m["above4k"], "%.2f" % m["above8k"], "%.2f" % m["above16k"]]
        print("\t".join(row))


if __name__ == "__main__":
    main()
