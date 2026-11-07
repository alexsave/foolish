"""Discard and pickup at a card game's pace, from an/disc_syn.py's parts (Alex 2026-11-07: "discard has 'scrapes' then a
flip as well, then a small soft placement of cards into the discard pile"; "for timing, just use reasonable timings").
DISCARD: the hand sweeps the cards together in strokes (the recordings: 4-5 strokes for about 10 cards, so one stroke per
  ~2.5 cards, 1-5), each stroke 0.8x a recorded one's length (0.25-0.7 s), 60-200 ms apart (recorded 50-200 ms), a little
  louder as the pile grows; the lift 100-180 ms after the last stroke; 500 ms in the air (ANIM_TIME_MS); the placement.
PICKUP: the same strokes and lift, then (Alex: "a bit more time between flip and put into hand") 550-700 ms later 2-3
  quiet taps as the cards go into the hand (../cover's cover3, 10 dB under a cover).
Levels: the lift at g-5 (the trump flip's level), on the deal's scale g = -34..-32 dBFS; the strokes 20 dB and the
placement 14 dB under it, as recorded (peaks: scrapes -37, lift -17, placements -31 dBFS).
Usage (from ../discard): python3 -I an/disc_game.py discard|pickup NAME SEED CARDS"""
import sys, os, glob, wave, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE); sys.argv, ARGS = sys.argv[:1], sys.argv
import disc_syn as D
KIND, NAME, SEED, CARDS = ARGS[1], ARGS[2], int(ARGS[3]), int(ARGS[4])
rng = np.random.default_rng(SEED); SR = D.SR
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def put(out, x, at, ref, db):
    x = x / (np.abs(D.bp(x, 150, 15000)).max() + 1e-12) * 10 ** (db / 20); k = int(at * SR) - ref; lo = max(0, k); hi = min(len(out), k + len(x)); out[lo:hi] += x[lo - k: hi - k]
out = np.zeros(12 * SR); g = rng.uniform(-34, -32); LIFT_DB = g - 5
strokes = int(np.clip(round(CARDS / 2.5), 1, 5)); T = 0.4
for s in range(strokes):
    dur = float(np.clip(rng.choice(D.SCRD) * 0.8 * rng.uniform(0.9, 1.1), 0.25, 0.7))
    y = D.scrape(rng, dur, pre=0.06); put(out, y, T, int(0.06 * SR), LIFT_DB - 20 + 3 * s / max(1, strokes - 1) - rng.uniform(0, 1.5))
    T += dur + (rng.uniform(0.06, 0.2) if s < strokes - 1 else 0)
T += rng.uniform(0.10, 0.18); y = D.lift(rng); put(out, y, T, int(0.08 * SR), LIFT_DB)
if KIND == "discard":
    T += 0.5; y = D.place(rng); put(out, y, T, int(0.08 * SR), LIFT_DB - 14); T += 0.35
else:
    taps = sorted(glob.glob(os.path.join(HERE, "..", "..", "cover", "synth", "cover3_*.wav"))); T += rng.uniform(0.55, 0.7)
    for _ in range(int(rng.integers(2, 4))):
        x = load(taps[rng.integers(len(taps))]); put(out, x, T, int(np.argmax(np.abs(x))), g - rng.uniform(14, 16)); T += rng.uniform(0.06, 0.11)
    T += 0.3
out = out[: int(T * SR)]
with wave.open(os.path.join(HERE, "..", "synth", f"{NAME}.wav"), "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(out, -1, 1) * 32767).astype(np.int16).tobytes())
print("wrote", NAME, f"{len(out) / SR:.2f} s, {strokes} strokes, peak {20 * np.log10(np.abs(out).max()):.1f} dBFS")
