#!/usr/bin/env python3
"""analyse_loop.py RUN_DIR LABEL PNG_PATH - the loop checks of one rendered held note (render_one.sh output).
Reads RUN_DIR/full.wav and RUN_DIR/trace.txt. The note is analysed from 0.3 s after its start to 50 ms before its note-off.
Prints one tab-separated line: label, sample file, sample length ms, loop zone ms, loop period ms (mean spacing of the joins),
joins in the sounding part, envelope autocorrelation peak (10 ms RMS dB, mean removed, lags 0.1 to 2 s) and its lag ms and
whether it sits at a multiple of the loop period (within 5 percent), f0-curve autocorrelation peak and lag, worst join level
dip (10 ms RMS around the join against the median of the 10 ms RMS in the 100 ms on both sides, dB), worst join flux ratio
(largest spectral flux within 20 ms of a join over the median flux of the note), f0 range (cents, 5th to 95th percentile).
Writes the f0 over time (0.25 s steps) to PNG_PATH with .f0.txt and the spectrogram (0 to 8 kHz, the 5 s from the note start).
Deterministic."""
import sys, re, zlib, struct
import numpy as np
sys.path.insert(0, "/Users/alex/Dev/foolish-wt-city-songs-v5/foolish/tools/soundtrack/tools/samples")
import lyria_notes_extract as le


def png(path, img):
    h, w, _ = img.shape
    raw = b"".join(b"\x00" + img[y].tobytes() for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def cmap(v):
    """v in 0..1 to an inferno-like colour."""
    stops = np.array([[0, 0, 4], [40, 11, 84], [101, 21, 110], [159, 42, 99], [212, 72, 66], [245, 125, 21], [250, 193, 39], [252, 255, 164]], float)
    x = np.clip(v, 0, 1) * (len(stops) - 1)
    i = np.minimum(x.astype(int), len(stops) - 2)
    f = (x - i)[..., None]
    return (stops[i] * (1 - f) + stops[i + 1] * f).astype(np.uint8)


def autocorr_peak(v, lo, hi):
    v = v - np.mean(v)
    n = len(v)
    f = np.fft.rfft(v, 2 * n)
    ac = np.fft.irfft(f * np.conj(f))[:n]
    ac = ac / max(ac[0], 1e-12)
    a, b = int(lo / 0.01), min(int(hi / 0.01), n - 1)
    if b <= a:
        return 0.0, 0.0
    k = a + int(np.argmax(ac[a:b + 1]))
    return float(ac[k]), k * 0.01


def main():
    d, label, pngp = sys.argv[1:4]
    tr = open(d + "/trace.txt").read().split("\n")
    note = [l for l in tr if l.startswith("TRACE note")][0]
    kv = dict(re.findall(r"(\w+)=(\S+)", note))
    t_on, dur, ratio, srs, ns = float(kv["t"]), float(kv["dur"]), float(kv["ratio"]), float(kv["sr"]), int(kv["n"])
    joins, zone = [], None
    for l in tr:
        m = re.match(r"TRACE wrap t=(\S+) la=(\d+) lb=(\d+) xf=(\d+)", l)
        if m:
            t, la, lb, xf = float(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
            joins.append((t - xf / ratio / 48000.0 / 2, xf / ratio / 48000.0))
            zone = (la / srs * 1000, lb / srs * 1000)
        m = re.match(r"TRACE join t=(\S+) xf_s=(\S+) A=(\d+) B=(\d+)", l)
        if m:
            joins.append((float(m.group(1)) - float(m.group(2)) / 2, float(m.group(2))))
            zone = (int(m.group(3)) / srs * 1000, int(m.group(4)) / srs * 1000)
    x, sr = le.decode(d + "/full.wav")
    m = x.mean(axis=1)
    a0, a1 = t_on + 0.3, t_on + dur - 0.05
    db, _ = le.level_db(m, sr)
    env = db[int(a0 / 0.01):int(a1 / 0.01)]
    jw = [j for j in joins if a0 <= j[0] <= a1]
    period = float(np.mean(np.diff([j[0] for j in jw]))) * 1000 if len(jw) > 1 else 0.0
    ep, el = autocorr_peak(env, 0.1, 2.0)
    f0, cen = le.yin_f0(m[int(a0 * sr):int(a1 * sr)], sr)
    ok = f0 > 0
    if ok.sum() > 10:
        cents = 1200 * np.log2(np.where(ok, f0, np.median(f0[ok])) / np.median(f0[ok]))
        tt = np.arange(len(cents))
        cents = np.interp(tt, tt[ok], cents[ok])
        fp, fl = autocorr_peak(cents, 0.1, 2.0)
        p5, p95 = np.percentile(cents[ok], [5, 95])
    else:
        fp = fl = p5 = p95 = 0.0
        cents = np.zeros(1)
    def at_multiple(lag):
        if period <= 0:
            return "n/a"
        k = round(lag * 1000 / period)
        return "yes" if k >= 1 and abs(lag * 1000 - k * period) <= 0.05 * period else "no"
    w = int(0.010 * sr)
    dips = []
    for (jt, xf) in jw:
        c = int(jt * sr)
        rc = 10 * np.log10(max(np.mean(m[c - w // 2:c + w // 2] ** 2), 1e-14))
        ref = []
        for lo, hi in ((jt - 0.1, jt - 0.02), (jt + 0.02, jt + 0.1)):
            for s in np.arange(lo, hi, 0.01):
                i = int(s * sr)
                ref.append(10 * np.log10(max(np.mean(m[i:i + w] ** 2), 1e-14)))
        dips.append(rc - float(np.median(ref)))
    flux, ft = le.spectral_flux(m, sr)
    sel = (ft >= a0) & (ft <= a1)
    medf = float(np.median(flux[sel])) if sel.any() else 1.0
    fr = [float(flux[(ft >= jt - 0.02) & (ft <= jt + 0.02)].max()) / max(medf, 1e-12) for jt, _ in jw if ((ft >= jt - 0.02) & (ft <= jt + 0.02)).any()]
    print("\t".join(map(str, [label, kv["file"].split("/")[-1], "%.0f" % (ns / srs * 1000), "%.0f-%.0f" % zone if zone else "-",
          "%.0f" % period, len(jw), "%.2f" % ep, "%.0f" % (el * 1000), at_multiple(el), "%.2f" % fp, "%.0f" % (fl * 1000),
          "%+.1f" % max(dips, key=abs) if dips else "-", "%.1f" % max(fr) if fr else "-", "%.0f" % (p95 - p5)])))
    with open(pngp.replace(".png", ".f0.txt"), "w") as f:
        f.write("f0 cents re median, every 0.25 s from %.2f s after the note start: %s\n" % (0.3, " ".join("%+.0f" % cents[i] for i in range(0, len(cents), 25))))
    # spectrogram 0 to 8 kHz over the 5 s from the note start
    seg = m[int(t_on * sr):int((t_on + 5.0) * sr)]
    N, H = 2048, 512
    nf = (len(seg) - N) // H + 1
    S = np.abs(np.fft.rfft(np.stack([seg[i * H:i * H + N] * np.hanning(N) for i in range(nf)]), axis=1)).T
    S = S[: int(8000 / (sr / N))]
    L = 20 * np.log10(np.maximum(S, 1e-9))
    L = (L - (L.max() - 80)) / 80
    img = cmap(L[::-1])
    img = np.repeat(img, 2, axis=1)
    png(pngp, np.ascontiguousarray(img))


main()
