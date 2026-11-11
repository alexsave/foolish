"""A pass at the game's timing (Alex 2026-11-02): "2 finger grab, slide out, throw in landing, and then a shfff as you
slide": the grab and the thumb slide before the card leaves, 500 ms in the air, the throw-in landing, then the pass
slide 150 ms after it. Levels on the deal's scale g; the shfff at g-(11..13). Usage: pass_game.py SOUNDGEN NAME SEED COUNT GAP"""
import sys, wave, glob, numpy as np
SR = 48000; SG, NAME = sys.argv[1], sys.argv[2]; rng = np.random.default_rng(int(sys.argv[3])); COUNT = int(sys.argv[4]); GAP = float(sys.argv[5])
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
def put(out, x, at, ref, db):
    x = x / (np.abs(x).max() + 1e-12) * 10 ** (db / 20); k = int(at * SR) - ref; lo = max(0, k); hi = min(len(out), k + len(x)); out[lo:hi] += x[lo - k: hi - k]
grabs = [load(p) for p in sorted(glob.glob(f"{SG}/throwin/synth/tgrab2_[0-9]*.wav"))]; slides = [load(p) for p in sorted(glob.glob(f"{SG}/deal/synth/thumb9_*.wav"))]
lands = [load(p) for p in sorted(glob.glob(f"{SG}/throwin/synth/throw9_*.wav"))]; shf = [load(p) for p in sorted(glob.glob("pass/synth/pass7_*.wav"))]
out = np.zeros(int((1.5 + COUNT * GAP + 1.0) * SR))
for i in range(COUNT):
    g = rng.uniform(-34, -32); T = 0.8 + i * GAP
    s = slides[rng.integers(len(slides))]; put(out, s, T - 0.13, int(np.argmax(np.abs(s))), g - rng.uniform(6, 9))
    gr = grabs[rng.integers(len(grabs))]; put(out, gr, T - 0.24, int(0.2 * SR), g - 14)
    l = lands[rng.integers(len(lands))]; put(out, l, T + 0.5, int(np.argmax(np.abs(l))), g - rng.uniform(3, 5))
    p = shf[rng.integers(len(shf))]; put(out, p, T + 0.65, int(0.1 * SR), g - rng.uniform(11, 13) + 20 * np.log10(np.abs(p).max() / (np.abs(p).max() + 1e-12)))   # onset at 0.1 s; level by its peak
save(f"synth/{NAME}.wav", out); print("wrote", NAME)
