"""The throw-in at the game's timing (Alex 2026-11-02): two fingers grab a card in the hand, the thumb slides it out of
the other cards (the deal's thumb slide), it leaves the hand, flies ANIM_TIME_MS = 500 ms and lands (the aggressive
throw-in, ANIM_CUE_PLACE struck at the landing). The grab sits right before the slide; the slide's peak is SLIDE_LEAD
before the card leaves, as in the deal. Levels on the deal's scale g: grab g-14 (very quiet), slide g-(6..9) as in the
deal, landing g-(3..5) (louder than a dealt card's landing: it is thrown).
Usage: throwin_game.py SOUNDGEN NAME SEED COUNT GAP_S"""
import sys, wave, glob, numpy as np
SR = 48000; SG, NAME = sys.argv[1], sys.argv[2]; rng = np.random.default_rng(int(sys.argv[3])); COUNT = int(sys.argv[4]); GAP = float(sys.argv[5])
SLIDE_LEAD, GRAB_BEFORE_SLIDE, FLIGHT = 0.13, 0.11, 0.500
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
def put(out, x, at, ref, db):
    x = x / (np.abs(x).max() + 1e-12) * 10 ** (db / 20); k = int(at * SR) - ref; lo = max(0, k); hi = min(len(out), k + len(x)); out[lo:hi] += x[lo - k: hi - k]
grabs = [load(p) for p in sorted(glob.glob("synth/tgrab2_[0-9]*.wav"))]
slides = [load(p) for p in sorted(glob.glob(f"{SG}/deal/synth/thumb9_*.wav"))]
lands = [load(p) for p in sorted(glob.glob("synth/throw9_*.wav"))]
out = np.zeros(int((1.5 + COUNT * GAP + 1.0) * SR))
for i in range(COUNT):
    g = rng.uniform(-34, -32); T = 0.8 + i * GAP                      # T: the card leaves the hand
    s = slides[rng.integers(len(slides))]; put(out, s, T - SLIDE_LEAD, int(np.argmax(np.abs(s))), g - rng.uniform(6, 9))
    gr = grabs[rng.integers(len(grabs))]; put(out, gr, T - SLIDE_LEAD - GRAB_BEFORE_SLIDE, int(0.2 * SR), g - 14)
    l = lands[rng.integers(len(lands))]; put(out, l, T + FLIGHT, int(np.argmax(np.abs(l))), g - rng.uniform(3, 5))
save(f"synth/{NAME}.wav", out); print("wrote", NAME)
