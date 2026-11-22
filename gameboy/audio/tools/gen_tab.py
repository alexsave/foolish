#!/usr/bin/env python3
# gen_tab.py - write snd_tab.c, the driver's tables (DESIGN.md "The tables").
# A non-production tool: the Game Boy build compiles its output, never runs it.
#
#   python3 -I tools/gen_tab.py WEBWAV CUE_TAKES_C JINGLES OUT_C BUILD [TICK_HZ]
#
# The card cues are FITTED to the website's real takes (soundgen's WAVs):
# each take's window (the part the Game Boy has time for, below) is cut into
# ticks, and each tick becomes one noise row, the LFSR clock whose spectrum
# is nearest the tick's and the volume that gives its level. The tunes come
# from jingles.txt. Also written, for the renderer and the comparison:
#   BUILD/target/<cue>_<k>.wav  the window of the real take the fit aimed at
#   BUILD/pcm/<cue>_<k>.pcm     approach B: the same window as 4-bit PCM at
#                               8192 Hz (a fixed layout: u16 sample count,
#                               u8 NR32 level code, then two samples a byte,
#                               high nibble first)
# No randomness: every byte is a pure function of the inputs.
import sys, os, re, math, wave, struct
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyse import read_wav, anchors  # noqa: E402

TICK_HZ = 60.0
CUE_IDS = dict(DEAL=1, FLIP=2, LIFT=3, PLACE=4, COVER=5, PICKUP=6, SWEEP=7, PILE=8, OUT=9, REVERT=10, GOOD=11,
               HAND_OFF=12, FINISH=13, FOOL=14, DEAL_LAND=15, TURN=16, PASS=17, INTO_HAND=18, BURST=19,
               FINISH_MIAMI=20, FINISH_NEW_YORK=21, FINISH_SEOUL=22, FINISH_MOSCOW=23)
N_CUES = 24

# The card cues: the takes fitted (three of the eight: variety without the
# ROM), how much of the gesture before its moment is kept (pre, ms), the most
# after it (post, ms), a part played before it (a stroke: the scrape a sweep
# or a pickup starts with), and the priority (a landing outranks a lift).
# pre is what the Game Boy has time for: a landing is known a whole flight
# (200 ms) ahead, so its pre-roll fits; a take-off cue starts as the button is
# pressed, so only the last 40-60 ms before its moment are kept (150 for the
# lift, whose thumb slide peaks 130 ms before the card leaves: it is heard 20
# ms after take-off), and the take's moment trails the card leaving by as
# much (trailing is the rule).
CARDS = [
    # cue        takes         pre post prefix                  prio
    ("DEAL",      "deal_tight", 50, 150, None,                   1),
    ("DEAL_LAND", "deal_land",  30, 150, None,                   2),
    ("TURN",      "turn",       40, 200, None,                   2),
    ("FLIP",      "flip",       30, 520, None,                   2),
    ("LIFT",      "lift",      150,  60, None,                   1),
    ("PLACE",     "place",      30, 200, None,                   2),
    ("COVER",     "cover",      30, 200, None,                   2),
    ("PASS",      "pass",        0, 450, None,                   1),
    ("PICKUP",    "pickup",     20, 200, ("pickup_strokes_s1", 180), 2),
    ("INTO_HAND", "into_hand",  20, 420, None,                   2),   # 2, not 1: struck while the pickup (2) still sounds, a 1 was dropped (README "Sound")
    ("SWEEP",     "sweep",      20, 200, ("sweep_strokes_s1", 220), 2),
    ("PILE",      "pile",       20, 250, None,                   2),
]
N_TAKES = 3

# Level: the Game Boy's span is narrower than the website's (4-bit volumes,
# a small speaker), so the website's loudest 50 ms RMS is mapped onto
# GB = -14 + 0.6 (web + 20) dBFS: the flip's -20 sits at -14, a slide into
# the hand's -49 at -31.
def gb_level_db(web_db):
    return -14.0 + 0.6 * (web_db + 20.0)


def db(v):
    return 20 * np.log10(np.maximum(v, 1e-12))


def fade(x, sr, ms=4.0):
    n = min(len(x) // 2, int(sr * ms / 1000))
    if n > 1:
        w = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, n))
        x = x.copy(); x[:n] *= w; x[-n:] *= w[::-1]
    return x


def window(webwav, anc, stem, pre, post):
    sr, x = read_wav(os.path.join(webwav, stem + ".wav"))
    a = anc[stem]
    a0 = max(0.0, a - pre) if a >= 0 else 0.0
    start = int(a0 * sr / 1000)
    y = x[start:]
    # end: post ms after the moment, or where the 5 ms envelope is 40 dB under the peak for good
    end = int((max(a, 0) - a0 + post) * sr / 1000)
    y = y[: max(end, 1)]
    n5 = int(sr * 0.005)
    e = np.sqrt(np.convolve(y * y, np.ones(n5) / n5, mode="same"))
    above = np.nonzero(e > e.max() * 10 ** (-40 / 20))[0]
    if len(above):
        y = y[: above[-1] + n5]
    pre_ms = a - a0 if a >= 0 else 0.0
    return sr, fade(y, sr), pre_ms


def to_rate(x, sr, out_sr):
    # band-limited resampling by FFT (non-production; the clips are short)
    n = int(round(len(x) * out_sr / sr))
    X = np.fft.rfft(x)
    m = n // 2 + 1
    Y = np.zeros(m, dtype=complex)
    k = min(m, len(X))
    Y[:k] = X[:k]
    return np.fft.irfft(Y, n) * (n / len(x))


# ---- the noise channel's spectrum ------------------------------------------------------
# NR43 = s << 4 | width << 3 | r. The LFSR clocks at 262144 / (r' 2^s) Hz,
# r' = 0.5 for r = 0 (Pan Docs). Its output holds a pseudo-random level for
# one clock, so its power spectrum is T sinc^2(f T), T = 1 / clock.
EDGES = np.geomspace(60, 15000, 17)


def noise_candidates():
    seen = {}
    for s in range(14):
        for r in range(8):
            fc = 262144.0 / ((0.5 if r == 0 else r) * (1 << s))
            key = round(fc, 3)
            if key not in seen:
                seen[key] = (s << 4) | r
    out = []
    for fc, code in sorted(seen.items()):
        if fc < 200:
            continue
        f = np.linspace(1, 15000, 6000)
        T = 1.0 / fc
        psd = 2 * T * np.sinc(f * T) ** 2
        bands = np.array([psd[(f >= lo) & (f < hi)].sum() * (f[1] - f[0]) for lo, hi in zip(EDGES[:-1], EDGES[1:])])
        out.append((code, fc, bands))
    return out


CANDS = None


def band_powers(seg, sr):
    n = 1 << max(8, int(np.ceil(np.log2(len(seg)))))
    w = np.hanning(len(seg))
    X = np.abs(np.fft.rfft(seg * w, n)) ** 2
    f = np.fft.rfftfreq(n, 1 / sr)
    return np.array([X[(f >= lo) & (f < hi)].sum() for lo, hi in zip(EDGES[:-1], EDGES[1:])]) + 1e-20


def fit_ticks(y, sr, gain, tick_hz):
    """Each tick of the target: the LFSR clock whose spectrum is nearest its
    own (NR43, the clock, its power below 15 kHz) and the tick's RMS."""
    global CANDS
    if CANDS is None:
        CANDS = noise_candidates()
    hop = sr / tick_hz
    nt = int(math.ceil(len(y) / hop))
    out = []
    for t in range(nt):
        a, b = int(t * hop), int((t + 1) * hop)
        seg = y[a:b] * gain
        if len(seg) < 4:
            break
        rms = math.sqrt(float(np.mean(seg * seg)))
        # the shape from a 16 ms window around the tick
        c0 = max(0, int((a + b) / 2 - 0.008 * sr)); c1 = min(len(y), c0 + int(0.016 * sr))
        R = band_powers(y[c0:c1], sr)
        Rd = np.maximum(10 * np.log10(R / R.max()), -30)
        best, bestc = 1e30, None
        for code, fc, P in CANDS:
            Pd = np.maximum(10 * np.log10(P / P.max()), -30)
            off = np.mean(Rd - Pd)          # the best level offset between the shapes
            e = float(np.sum((Rd - Pd - off) ** 2))
            if e < best:
                best, bestc = e, (code, fc, P)
        code, fc, P = bestc
        out.append((code, fc, float(P.sum()), rms))
    return out


# The envelope a noise trigger can make: volume v0, then one step every
# pace/64 s up or down (the frame sequencer's 64 Hz), cut by the length
# counter after L/256 s. A trigger resets the LFSR, whose output then holds
# for 15 clocks (the reset run): silence, as far as the fit is concerned.
SUB = 4                     # sub-samples a tick
PARAMS = [(v0, d, p) for v0 in range(1, 16) for d in (-1, 1) for p in range(0, 8) if not (p == 0 and d == 1)]


def model_env(params, fc0, F, n, tick_hz, L=0):
    """The RMS a tick, for each (v0, dir, pace) in params, over n ticks."""
    t = (np.arange(n * SUB) + 0.5) / SUB / tick_hz
    P = np.array(params, dtype=float)
    v0, d, p = P[:, 0:1], P[:, 1:2], P[:, 2:3]
    steps = np.where(p > 0, np.floor(t[None, :] / np.maximum(p, 1) * 64.0), 0)
    vol = np.clip(v0 + d * steps, 0, 15)
    amp = vol / 60.0 * np.sqrt(np.repeat(F[:n], SUB))[None, :]
    amp[:, t < 15.0 / fc0] = 0
    if L:
        amp[:, t >= L / 256.0] = 0
    return np.sqrt(np.mean((amp ** 2).reshape(len(params), n, SUB), axis=2))


def fit_events(ticks, tick_hz, thr=2.0, maxn=60):
    """Noise rows (t, r0, NR41, NR42, NR43, NR44): one trigger an event, the
    clock changed live (no retrigger) inside it."""
    n_all = len(ticks)
    rms = np.array([x[3] for x in ticks])
    F = np.array([x[2] for x in ticks])
    tdb = 20 * np.log10(rms + 1e-9)
    floor = tdb.max() - 40
    tdb = np.maximum(tdb, floor)
    rows = []
    i = 0
    while i < n_all:
        if tdb[i] <= floor + 1:
            if rows and rows[-1][3] == 0 and rows[-1][0] < 255:
                rows[-1][0] += 1
            else:
                rows.append([1, 0, 0, 0, 0, 0])
            i += 1
            continue
        # A trigger at a slow clock would be silent for its reset run (15
        # clocks: 5.5 ms at 2.7 kHz, longer than a tick), so an event is
        # triggered at a clock fast enough that the run is under a fifth of a
        # tick, and moved live to the clock its spectrum wants from its second
        # tick on (the LFSR keeps running: no second reset).
        code0, fc0, F0 = ticks[i][0], ticks[i][1], ticks[i][2]
        if 15.0 / fc0 > 0.2 / tick_hz:
            code0, fc0, P0 = min((c for c in CANDS if c[1] >= 75.0 * tick_hz), key=lambda c: c[1])
            F0 = float(P0.sum())
        Fe = F[i:].copy()
        Fe[0] = F0
        best = None
        for n in range(1, min(maxn, n_all - i) + 1):
            if n > 1 and tdb[i + n - 1] - tdb[i + n - 2] > 6:
                break                       # an onset starts the next event
            m = model_env(PARAMS, fc0, Fe, n, tick_hz)
            e = np.mean(np.abs(np.maximum(20 * np.log10(m + 1e-9), floor) - tdb[i:i + n]), axis=1)
            k = int(np.argmin(e))
            if n == 1 or e[k] <= thr:
                best = (n, PARAMS[k], float(e[k]))
            elif e[k] > 2 * thr:
                break
        n, (v0, d, p), err = best
        L = 0
        # before a rest or the end, the length counter can cut inside a tick
        if i + n >= n_all or tdb[i + n] <= floor + 1:
            base = model_env([(v0, d, p)], fc0, Fe, n, tick_hz)
            be = float(np.mean(np.abs(np.maximum(20 * np.log10(base[0] + 1e-9), floor) - tdb[i:i + n])))
            for Lc in range(1, 64):
                if Lc / 256.0 > n / tick_hz:
                    break
                m = model_env([(v0, d, p)], fc0, Fe, n, tick_hz, Lc)
                e = float(np.mean(np.abs(np.maximum(20 * np.log10(m[0] + 1e-9), floor) - tdb[i:i + n])))
                if e < be - 0.3:
                    be, L = e, Lc
        env = (v0 << 4) | (0x08 if d > 0 else 0) | p
        nr41 = (64 - L) if L else 0
        nr44 = 0x40 if L else 0
        code = code0
        rows.append([1, 0, nr41, env, code, 0x80 | nr44])
        for j in range(i + 1, i + n):
            if ticks[j][0] == code and rows[-1][0] < 255:
                rows[-1][0] += 1
            else:
                code = ticks[j][0]
                rows.append([1, 0, 0, env, code, nr44])   # the clock only, live
        i += n
    while rows and rows[-1][3] == 0:
        rows.pop()
    return [tuple(r) for r in rows]


# ---- PCM (approach B) ------------------------------------------------------------------
PCM_HZ = 8192


def write_pcm(path, y, sr, gain):
    z = to_rate(y * gain, sr, PCM_HZ)
    # the DAC swings +-1 at full digital range, /4 by the mix: a sample s
    # (0..15) is (7.5 - s) / 7.5 / 4. Choose the NR32 level that keeps the most bits.
    pk = float(np.max(np.abs(z))) + 1e-12
    full = 0.25
    code, shift = 1, 0
    for c, sh in ((3, 2), (2, 1)):
        if pk * (1 << sh) <= full:
            code, shift = c, sh
            break
    q = np.clip(np.round(7.5 - z * (1 << shift) / full * 7.5), 0, 15).astype(np.uint8)
    if len(q) % 2:
        q = np.append(q, 7)
    with open(path, "wb") as f:
        f.write(struct.pack("<HB", len(q), code))
        f.write(bytes(((q[0::2] << 4) | q[1::2]).tolist()))
    return len(q) // 2 + 3


def write_wav(path, y, sr):
    z = np.clip(np.round(y * 32767), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr); w.writeframes(z.tobytes())


# ---- the tunes -------------------------------------------------------------------------
NOTE = {"C": -9, "D": -7, "E": -5, "F": -4, "G": -2, "A": 0, "B": 2}


def note_hz(n):
    m = re.fullmatch(r"([A-G])([#b]?)(-?\d)", n)
    semis = NOTE[m.group(1)] + (1 if m.group(2) == "#" else -1 if m.group(2) == "b" else 0) + 12 * (int(m.group(3)) - 4)
    return 440.0 * 2 ** (semis / 12)


def env_byte(vol, env):
    env = int(env)
    if env == 0:
        return (vol << 4) | 0x00 if vol else 0x08
    return (vol << 4) | (0x08 if env > 0 else 0) | min(7, abs(env))


# The tunes sit under the card set, as the website's placeholders were turned
# down to (AUDIO.md, open question 12: the game over just under a card
# landing): every pulse and noise volume of jingles.txt times this, and the
# wave channel one level quieter.
JINGLE_GAIN = 0.55


def parse_jingles(path, tick_hz):
    jv = lambda v: max(1, int(round(int(v) * JINGLE_GAIN)))
    cues = {}
    cur = take = None
    ch = None
    for raw in open(path):
        line = [] if raw.lstrip().startswith("#") else raw.split()
        if not line:
            continue
        op, a = line[0], line[1:]
        if op == "cue":
            cur = cues.setdefault(a[0], {"prio": int(a[1]), "takes": []})
        elif op == "take":
            take = {1: [], 2: [], 3: [], 4: []}
            cur["takes"].append(take)
        elif op == "ch":
            ch = int(a[0])
        elif op == "p":
            f = note_hz(a[0]); x = int(round(2048 - 131072 / f))
            sweep = int(a[5], 16) if len(a) > 5 else 0x08 if ch == 1 else 0
            take[ch].append((int(a[1]), sweep, int(a[4]) << 6, env_byte(jv(a[2]), a[3]), x & 0xFF, 0x80 | (x >> 8)))
        elif op == "g":
            f = note_hz(a[0]); x = int(round(2048 - 131072 / f))
            last = take[ch][-1]
            take[ch].append((int(a[1]), last[1], last[2], last[3], x & 0xFF, x >> 8))
        elif op == "w":
            f = note_hz(a[0]); x = int(round(2048 - 65536 / f))
            take[ch].append((int(a[1]), int(a[3]), 0, min(3, int(a[2]) + 1) << 5, x & 0xFF, 0x80 | (x >> 8)))
        elif op == "n":
            take[ch].append((int(a[1]), 0, 0, env_byte(jv(a[2]), a[3]), int(a[0], 16), 0x80))
        elif op == "r":
            take[ch].append((int(a[0]), 0, 0, 0, 0, 0))
        else:
            raise SystemExit("jingles: unknown line: " + raw)
    # jingles.txt counts in 1/241 s: to this table's ticks, rounding the
    # running time (not each row), so a rhythm does not drift
    k = tick_hz / 241.0
    for d in cues.values():
        for t in d["takes"]:
            for ch, rows in t.items():
                cum, conv = 0, []
                for r in rows:
                    a, b = int(round(cum * k)), int(round((cum + r[0]) * k))
                    conv.append((max(1, b - a),) + r[1:])
                    cum += r[0]
                t[ch] = conv
    # a row holds 255 ticks at most: a longer note goes on as a pitch-only row
    for d in cues.values():
        for t in d["takes"]:
            for ch, rows in t.items():
                out = []
                for r in rows:
                    n = r[0]
                    first = True
                    while n > 0:
                        k = min(255, n)
                        if first:
                            out.append((k,) + r[1:])
                        else:
                            out.append((k,) + r[1:4] + (r[4], r[5] & 0x7F) if r[3] else (k, 0, 0, 0, 0, 0))
                        first = False
                        n -= k
                t[ch] = out
    return cues


# The wave channel's tables: 0 a soft triangle (the bass).
WAVES = [[min(15, int(round(7.5 + 7.5 * (1 - abs((i / 16.0) - 1) * 2)))) for i in range(32)]]


def main():
    webwav, ctable, jpath, out_c, build = sys.argv[1:6]
    tick_hz = float(sys.argv[6]) if len(sys.argv) > 6 else TICK_HZ
    anc = anchors(ctable)
    os.makedirs(os.path.join(build, "target"), exist_ok=True)
    os.makedirs(os.path.join(build, "pcm"), exist_ok=True)
    # the website's loudness of each card cue: its loudest 50 ms RMS, mean over the takes fitted
    cue_rows = {}      # cue -> list of takes, a take = {ch: rows}
    prio, pre_ticks, dur_ticks, pcm_bytes = {}, {}, {}, 0
    meta = []
    for cue, stem, pre, post, prefix, pr in CARDS:
        takes = []
        lv = []
        wins = []
        for k in range(N_TAKES):
            sr, y, pre_ms = window(webwav, anc, "%s_%d" % (stem, k), pre, post)
            if prefix:
                psr, px = read_wav(os.path.join(webwav, "%s_%d.wav" % (prefix[0], k % 3)))
                assert psr == sr or True
                px = to_rate(px, psr, sr) if psr != sr else px
                # the stroke's loudest part, prefix[1] ms of it, then the knock
                n5 = int(sr * 0.005)
                e = np.convolve(px * px, np.ones(n5), mode="same")
                c = int(np.argmax(e)); L = int(sr * prefix[1] / 1000)
                a0 = max(0, c - L // 2)
                y = np.concatenate([fade(px[a0:a0 + L], sr, 15), y])
                pre_ms = 0.0
            wins.append((sr, y, pre_ms))
            n50 = int(sr * 0.05)
            lv.append(db(np.sqrt(np.max(np.convolve(y * y, np.ones(n50) / n50, mode="valid")))))
        web_db = float(np.mean(lv))
        target_db = gb_level_db(web_db)
        for k, (sr, y, pre_ms) in enumerate(wins):
            n50 = int(sr * 0.05)
            own = db(np.sqrt(np.max(np.convolve(y * y, np.ones(n50) / n50, mode="valid"))))
            gain = 10 ** ((target_db - web_db) / 20)
            write_wav(os.path.join(build, "target", "%s_%d.wav" % (cue.lower(), k)), y * gain, sr)
            ticks = fit_ticks(y, sr, gain, tick_hz)
            takes.append({4: fit_events(ticks, tick_hz)})
            pcm_bytes += write_pcm(os.path.join(build, "pcm", "%s_%d.pcm" % (cue.lower(), k)), y, sr, gain)
            meta.append((cue, k, len(y) / sr * 1000, pre_ms, web_db, target_db, own - web_db))
        cue_rows[cue] = takes
        prio[cue] = pr
        pre_ticks[cue] = int(round(np.mean([w[2] for w in wins]) * tick_hz / 1000))
        dur_ticks[cue] = max(sum(r[0] for r in t[4]) for t in takes)
    jing = parse_jingles(jpath, tick_hz)
    for cue, d in jing.items():
        cue_rows[cue] = d["takes"]
        prio[cue] = d["prio"]
        pre_ticks[cue] = 0
        dur_ticks[cue] = max(max((sum(r[0] for r in rows) for rows in t.values()), default=0) for t in d["takes"])
    # ---- write the C ----
    rom = 0
    L = []
    L.append("// GENERATED by gameboy/audio/tools/gen_tab.py from the website's takes")
    L.append("// (foolish tools/soundgen) and jingles.txt - do not edit; run `make -C gameboy/audio tables`.")
    L.append("// %d ticks a second." % tick_hz)
    L.append('#include "snd.h"')
    L.append("")
    names = sorted(cue_rows, key=lambda c: CUE_IDS[c])
    for cue in names:
        for k, t in enumerate(cue_rows[cue]):
            for ch in (1, 2, 3, 4):
                rows = t.get(ch) or []
                if not rows:
                    continue
                L.append("static const snd_row %s_%d_ch%d[] = {" % (cue.lower(), k, ch))
                for r in rows:
                    L.append("    { %3d, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X }," % r)
                L.append("    { 0, 0, 0, 0, 0, 0 },")
                L.append("};")
                rom += 6 * (len(rows) + 1)
        L.append("static const snd_take %s_takes[] = {" % cue.lower())
        for k, t in enumerate(cue_rows[cue]):
            vs = ["%s_%d_ch%d" % (cue.lower(), k, ch) if t.get(ch) else "0" for ch in (1, 2, 3, 4)]
            L.append("    { { %s } }," % ", ".join(vs))
            rom += 8
        L.append("};")
    L.append("")
    L.append("const snd_cue snd_cues[SND_N_CUES] = {")
    for cue in names:
        L.append("    [SND_%s] = { %d, %d, %s_takes }," % (cue, prio[cue], len(cue_rows[cue]), cue.lower()))
    L.append("};")
    rom += 4 * N_CUES
    L.append("")
    L.append("// ticks from a strike to its moment (the pre-roll): a landing is struck this much before it lands")
    L.append("const uint8_t snd_pre[SND_N_CUES] = {")
    L.append("    " + " ".join("[SND_%s] = %d," % (c, pre_ticks[c]) for c in names))
    L.append("};")
    L.append("// ticks from a strike to the end of its longest take")
    L.append("const uint8_t snd_dur[SND_N_CUES] = {")
    L.append("    " + " ".join("[SND_%s] = %d," % (c, min(255, dur_ticks[c])) for c in names))
    L.append("};")
    rom += 2 * N_CUES
    L.append("")
    L.append("const uint8_t snd_waves[][16] = {")
    for w in WAVES:
        L.append("    { " + ", ".join("0x%02X" % ((w[2 * i] << 4) | w[2 * i + 1]) for i in range(16)) + " },")
    L.append("};")
    rom += 16 * len(WAVES)
    L.append("// the tables above: %d bytes of ROM on the SM83 (6 a row, 8 a take, 4 a cue, 2 tables of %d)" % (rom, N_CUES))
    open(out_c, "w").write("\n".join(L) + "\n")
    with open(os.path.join(build, "tables.tsv"), "w") as f:
        f.write("cue\ttake\twindow_ms\tpre_ms\tweb_db\tgb_db\n")
        for m in meta:
            f.write("%s\t%d\t%.0f\t%.0f\t%.1f\t%.1f\n" % m[:6])
        f.write("ROM\t%d\tPCM_ROM\t%d\n" % (rom, pcm_bytes))
    print("gen_tab: %d bytes of tables, %d bytes of PCM (approach B, %d takes), %d Hz ticks" % (rom, pcm_bytes, len(meta), tick_hz))


if __name__ == "__main__":
    main()
