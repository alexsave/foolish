"""Discard and pickup, built from the pass's shfff (Alex 2026-11-07).
DISCARD: "a bunch of passes playing at the same time, with roughly a few instances of difference in where they start, and
all end in the same round. then several rounds", one shfff per card, "like a bunch of cards being swept on the table".
  The cards are split over STROKES sweeps of the hand. In a stroke the cards join in 2-3 clusters (the hand meets them
  where they lie), each cluster's start a little later, and every card stops when the hand stops: a card's shfff is the
  pass synth time-stretched to the distance it travels (the pass stops ~0.31*ts after its onset).
  The fingers land once per stroke, so only the stroke's first card has the landing knock; the cards riding on others
  are quieter (they rub card, not cloth) and their stop knocks are mostly one shared knock.
PICKUP: the same sweep, then a flip (the pile lifted and turned, ../flip's flipE), then a few quiet taps as the cards are
  squared into the hand (Alex: "similar to that of the cover sound, but quieter"): the cover landing, 10 dB under a cover.
Levels on the deal's scale g = -34..-32 dBFS: shfff g-(11..13) as in the pass, flip g-5, cover taps g-(14..16).
Usage (from ../sweep): python3 -I an/sweep_game.py discard|pickup NAME SEED CARDS [STROKES]
Timing (Alex: "reasonable timings as you'd expect in a card game"): a stroke 280-380 ms, 150-300 ms for the hand to go back,
the flip's snap 50-120 ms after the sweep stops, the first tap 280-360 ms after the snap, the taps 60-110 ms apart."""
import sys, os, glob, wave, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); SG = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(SG, "pass", "an")); import pass_syn as P      # the pass synth, its tables and rng
KIND, NAME = sys.argv[1], sys.argv[2]; SEED, CARDS = int(sys.argv[3]), int(sys.argv[4])
STROKES = int(sys.argv[5]) if len(sys.argv) > 5 else min(3, (CARDS + 3) // 4)   # a hand sweeps about 4 cards at a stroke
rng = np.random.default_rng(SEED); P.rng = np.random.default_rng(SEED + 1000); SR = P.SR; O = P.O
STOP = 0.31          # a pass's card stops ~0.31*ts after its onset (stop snags 0.275-0.345, stop knock 0.284-0.345)
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def put(out, x, at, ref, db):
    x = x / (np.abs(x).max() + 1e-12) * 10 ** (db / 20); k = int(at * SR) - ref; lo = max(0, k); hi = min(len(out), k + len(x)); out[lo:hi] += x[lo - k: hi - k]
out = np.zeros(int((2.0 + STROKES * 0.7 + 2.0) * SR))
per = np.full(STROKES, CARDS // STROKES); per[: CARDS % STROKES] += 1; rng.shuffle(per)     # cards per stroke
T = 0.5; log = []
for s in range(STROKES):
    n = int(per[s]); D = rng.uniform(0.28, 0.38); g = rng.uniform(-34, -32)                      # D: the stroke, first card's travel
    ncl = min(n, int(rng.integers(2, 4))); offs = np.r_[0, np.sort(rng.uniform(0.04, 0.55 * D, ncl - 1))]
    cl = np.r_[np.arange(ncl), rng.integers(0, ncl, n - ncl)]                                     # every cluster has a card
    for c in range(n):
        off = offs[cl[c]] + (rng.uniform(-0.006, 0.006) if c else 0); ts = max(0.35, (D - off) / STOP)
        lead = c == 0
        y = P.slide(ts=ts, land_knock=lead, stop_db=0 if lead else -12)
        on = T + D - STOP * ts + rng.uniform(-0.003, 0.003) * (not lead)                          # all stop with the hand
        db = g - rng.uniform(11, 13) - (0 if lead else rng.uniform(3, 6))
        put(out, y, on, O, db + 20 * np.log10(np.abs(y).max()) - 20 * np.log10(np.abs(y).max()))
        log.append((s, round(on - T, 3), round(ts, 2)))
    T += D + rng.uniform(0.15, 0.30)                                                               # the hand goes back
end = T
if KIND == "pickup":
    flips = sorted(glob.glob(os.path.join(SG, "flip", "synth", "flipE_[0-9]*.wav"))); taps = sorted(glob.glob(os.path.join(SG, "cover", "synth", "cover3_*.wav")))
    g = rng.uniform(-34, -32); snap = end + rng.uniform(0.05, 0.12)
    put(out, load(flips[rng.integers(len(flips))]), snap, int(0.35 * SR), g - 5)
    t = snap + rng.uniform(0.28, 0.36)
    for _ in range(int(rng.integers(2, 4))):
        x = load(taps[rng.integers(len(taps))]); put(out, x, t, int(np.argmax(np.abs(x))), g - rng.uniform(14, 16)); t += rng.uniform(0.06, 0.11)
    end = t
out = out[: int((end + 0.7) * SR)]
with wave.open(os.path.join(HERE, "..", "synth", f"{NAME}.wav"), "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(out, -1, 1) * 32767).astype(np.int16).tobytes())
print("wrote", NAME, f"{len(out) / SR:.2f} s, peak {20 * np.log10(np.abs(out).max()):.1f} dBFS, cards per stroke", [int(v) for v in per])
