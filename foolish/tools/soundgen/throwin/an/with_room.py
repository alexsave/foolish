"""For measuring only: the synth grabs with IMG_2952's own room under them, so the tails compare like for like."""
import sys, glob, wave, numpy as np
SR = 48000; rng = np.random.default_rng(1)
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
room = load("parts/grab_room.wav")
for p in sorted(glob.glob(f"synth/{sys.argv[1]}_*.wav")):
    x = load(p); r = np.tile(room, 2)[: len(x)]; a = rng.integers(0, len(room) // 2); r = np.roll(r, a)
    save(p.replace(f"{sys.argv[1]}_", f"{sys.argv[1]}R_"), x + r)
