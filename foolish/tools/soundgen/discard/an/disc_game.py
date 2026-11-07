"""Discard and pickup at a card game's pace, from an/disc_syn.py's parts (Alex 2026-11-07: "discard has 'scrapes' then a
flip as well, then a small soft placement of cards into the discard pile"; "for timing, just use reasonable timings").
DISCARD: the hand sweeps the cards together in strokes (the recordings: 4-5 strokes for about 10 cards, so one stroke per
  ~2.5 cards, 1-5), each stroke 0.8x a recorded one's length (0.25-0.7 s), 60-200 ms apart (recorded 50-200 ms), a little
  louder as the pile grows; the lift 100-180 ms after the last stroke; 500 ms in the air (ANIM_TIME_MS); the placement.
PICKUP (IMG_3052-3054): the same strokes, then the pile lifted off the table with one soft knock, then after the 500 ms
  flight to the hand (Alex: "a bit more time between flip and put into hand"; "there should just be one tap into the
  hand, and it should be quieter") one slide as the cards go into the fanned hand.
Levels by 50 ms RMS against the strokes, as recorded: the discard's lift +5 dB (its three videos: -3, +11, +23, so the
  first version's +17 was far too loud) and its placement +3 dB; the pickup's lift -10 dB and its slide into the hand
  -11 dB. The strokes sit at g-27 (so their peaks land near a pass's g-12), on the deal's scale g = -34..-32 dBFS.
Usage (from ../discard): python3 -I an/disc_game.py discard|pickup NAME SEED CARDS"""
import sys, os, glob, wave, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE); sys.argv, ARGS = sys.argv[:1], sys.argv
import disc_syn as D
KIND, NAME, SEED, CARDS = ARGS[1], ARGS[2], int(ARGS[3]), int(ARGS[4])
rng = np.random.default_rng(SEED); SR = D.SR
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def put(out, x, at, ref, db):   # db: the part's loudest 50 ms RMS
    y = D.bp(x, 150, 15000); x = x / (np.sqrt(np.convolve(y ** 2, np.ones(2400) / 2400, "same")).max() + 1e-12) * 10 ** (db / 20); k = int(at * SR) - ref; lo = max(0, k); hi = min(len(out), k + len(x)); out[lo:hi] += x[lo - k: hi - k]
out = np.zeros(12 * SR); g = rng.uniform(-34, -32); SC = g - 27
strokes = int(np.clip(round(CARDS / 2.5), 1, 5)); T = 0.4
for s in range(strokes):
    dur = float(np.clip(rng.choice(D.SCRD) * 0.8 * rng.uniform(0.9, 1.1), 0.25, 0.7))
    y = D.scrape(rng, dur, pre=0.06); put(out, y, T, int(0.06 * SR), SC - 1.5 + 3 * s / max(1, strokes - 1) - rng.uniform(0, 1.5))
    T += dur + (rng.uniform(0.06, 0.2) if s < strokes - 1 else 0)
if KIND == "discard":
    T += rng.uniform(0.10, 0.18); y = D.lift(rng); put(out, y, T, int(0.08 * SR), SC + 5)
    T += 0.5; y = D.place(rng); put(out, y, T, int(0.08 * SR), SC + 3); T += 0.35
else:
    T += rng.uniform(0.05, 0.12); y = D.plift(rng); put(out, y, T, int(0.08 * SR), SC - 10)
    T += rng.uniform(0.5, 0.6); y = D.insert(rng); put(out, y, T, int(0.06 * SR), SC - 11); T += 0.06 + len(y) / SR
out = out[: int(T * SR)]
with wave.open(os.path.join(HERE, "..", "synth", f"{NAME}.wav"), "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(out, -1, 1) * 32767).astype(np.int16).tobytes())
print("wrote", NAME, f"{len(out) / SR:.2f} s, {strokes} strokes, peak {20 * np.log10(np.abs(out).max()):.1f} dBFS")
