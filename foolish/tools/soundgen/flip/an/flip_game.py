"""The trump flip at the game's timing: the step starts as the card leaves the stock (the flip's snap), flies
ANIM_TIME_MS = 500 ms and lands (ANIM_CUE_FLIP, struck at the landing): the landing is a throw-in (Alex: "it goes on
the table, similar to a throw in"), then the deck is set down on top of it DECK_AFTER later (no game event yet; a guess).
Usage: flip_game.py NAME SEED COUNT GAP_S. Levels on the deal's scale (an/deal_game.py: g = -34..-32 dBFS):
the flip at g-5 (the grab's level), the landing at g-12..-9 (a single card, low), the deck at g-6..-3 (the heaviest)."""
import sys, wave, glob, numpy as np
SR = 48000; NAME = sys.argv[1]; rng = np.random.default_rng(int(sys.argv[2])); COUNT = int(sys.argv[3]); GAP = float(sys.argv[4])
FLIGHT, DECK_AFTER = 0.500, 0.350
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
flips = [load(p) for p in sorted(glob.glob("synth/flipE_[0-9]*.wav"))]       # snap at 0.35 s
decks = [load(p) for p in sorted(glob.glob("synth/deck229c_*.wav"))]            # thud at 0.2 s
lands = [load(p) for p in sorted(glob.glob("../throwin/synth/throw9_*.wav"))]        # peak near 0.06 s
def put(out, x, at_s, ref_idx, db):
    x = x / (np.abs(x).max() + 1e-12) * 10 ** (db / 20); k = int(at_s * SR) - ref_idx; lo = max(0, k)
    out[lo:k + len(x)] += x[lo - k: len(out) - k] if k + len(x) > len(out) else x[lo - k:]
out = np.zeros(int((1.0 + COUNT * GAP + 1.5) * SR))
for i in range(COUNT):
    g = rng.uniform(-34, -32); T = 0.5 + i * GAP
    f = flips[rng.integers(len(flips))]; put(out, f, T, int(0.35 * SR), g - 5)
    l = lands[rng.integers(len(lands))]; put(out, l, T + FLIGHT, int(np.argmax(np.abs(l))), g - rng.uniform(9, 12))
    d = decks[rng.integers(len(decks))]; put(out, d, T + FLIGHT + DECK_AFTER, int(0.2 * SR), g - rng.uniform(3, 6))
save(f"synth/{NAME}.wav", out); print("wrote", NAME)
