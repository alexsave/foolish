"""A 4-player opening and three attack/cover pairs, every sound from its model, on the game's timing:
the deal (deal/synth/deal_4p_seats.wav: 24 cards from 0.6 s at 375 ms), the trump flip at the next step (flip, a throw-in
landing at +500 ms, the deck 350 ms later), then attacks and covers, each one card: the two-finger grab, the thumb slide,
the card leaving the hand, 500 ms in the air, then the landing (a throw-in for an attack, the cover landing for a cover)."""
import sys, wave, glob, numpy as np
SR = 48000; rng = np.random.default_rng(11); SG = sys.argv[1]
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
def put(out, x, at, ref, db):
    x = x / (np.abs(x).max() + 1e-12) * 10 ** (db / 20); k = int(at * SR) - ref; lo = max(0, k); hi = min(len(out), k + len(x)); out[lo:hi] += x[lo - k: hi - k]
deal = load(f"{SG}/deal/synth/deal_4p_seats.wav")
flips = sorted(glob.glob(f"{SG}/flip/synth/flipE_[0-9]*.wav")); decks = sorted(glob.glob(f"{SG}/flip/synth/deck229c_*.wav"))
throws = sorted(glob.glob(f"{SG}/throwin/synth/throw9_*.wav")); grabs = sorted(glob.glob(f"{SG}/throwin/synth/tgrab2_[0-9]*.wav"))
slides = sorted(glob.glob(f"{SG}/deal/synth/thumb9_*.wav")); covs = sorted(glob.glob("cover/synth/cover3_*.wav"))
out = np.zeros(int(26 * SR)); out[:len(deal)] += deal; g = -33.0
T = 0.6 + 24 * 0.375
f = load(flips[3]); put(out, f, T, int(0.35 * SR), g - 5)
l = load(throws[4]); put(out, l, T + 0.5, int(np.argmax(np.abs(l))), g - 10.5)
d = load(decks[1]); put(out, d, T + 0.85, int(0.2 * SR), g - 4.5)
T += 0.85 + 1.6
def card(T, landing, ldb):
    s = load(slides[rng.integers(len(slides))]); put(out, s, T - 0.13, int(np.argmax(np.abs(s))), g - rng.uniform(6, 9))
    gr = load(grabs[rng.integers(len(grabs))]); put(out, gr, T - 0.24, int(0.2 * SR), g - 14)
    l = load(landing); put(out, l, T + 0.5, int(np.argmax(np.abs(l))), g - rng.uniform(*ldb))
for i in range(3):
    card(T + 0.3, throws[(3 + i) % 12], (3, 5)); T += 0.8 + rng.uniform(1.0, 1.6)
    card(T + 0.3, covs[(2 * i + 1) % 12], (4, 6)); T += 0.8 + rng.uniform(1.2, 1.8)
save("synth/round_demo2.wav", out[: int((T + 1.0) * SR)]); print("wrote round_demo2", round(T + 1.0, 1), "s")
