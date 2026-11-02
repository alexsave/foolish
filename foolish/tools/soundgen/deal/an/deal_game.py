"""Deals at the GAME's internal timing (ANIM_DEAL_CARD_MS = 350): grab at T (the card leaves), landing at T + 350 ms
(it lands on screen), the thumb slide's peak SLIDE_LEAD before the grab so its audible part is over by the click.
Deals start PACE apart. Every part is placed by its own peak. Levels: grab -24..-26 dBFS, slide 6-9 dB under, landing 11-14 dB under."""
import sys, wave, numpy as np
rng = np.random.default_rng(int(sys.argv[3]) if len(sys.argv) > 3 else 9); SR = 48000
pace, name = float(sys.argv[1]), sys.argv[2]; N = int(sys.argv[4]) if len(sys.argv) > 4 else 6
SEATS = int(sys.argv[5]) if len(sys.argv) > 5 else 0      # 0: no seats (every landing at full level)
# the landing's level follows its distance from the listener (inverse-distance law, -6 dB per doubling): the seats sit on
# a circle of radius R round the dealer at the centre; the listener is seat 0, whose own cards land OWN_D in front of them
# 3D: the listener's head is HEAD_H above the table at their seat; their own cards land OWN_IN in front of them
R, HEAD_H, OWN_IN = 0.5, 0.40, 0.15
def seat_dist(k):
    th = 2 * np.pi * (k % SEATS) / SEATS
    sx, sy = R * np.cos(th), R * np.sin(th)                          # where seat k's cards land (seat 0 at (R, 0))
    if k % SEATS == 0: sx = R - OWN_IN                                 # the listener's own cards, just in front of them
    else: sx, sy = (R - OWN_IN) * np.cos(th), (R - OWN_IN) * np.sin(th)
    return np.sqrt((sx - R) ** 2 + sy ** 2 + HEAD_H ** 2)            # from the head at (R, 0, HEAD_H)
def seat_gain_db(k):
    if SEATS == 0: return 0.0
    return 20 * np.log10(seat_dist(0) / seat_dist(k))
SLIDE_LEAD, FLIGHT = 0.13, 0.35
GRAB_TRIM = 5.0   # Alex: the grab quieter (the slide and landing stay where they were)
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
def place(out, x, peak_time, db):
    y = x / np.abs(x).max() * 10 ** (db / 20); k = int(peak_time * SR) - int(np.argmax(np.abs(x)))
    lo = max(0, k); out[lo: k + len(y)] += y[lo - k: len(out) - k] if k + len(y) <= len(out) else y[lo - k: len(out) - k]
out = np.zeros(int((N * pace + 2) * SR)); T = 0.6
for i in range(N):
    g = rng.uniform(-34, -32)
    place(out, load(f"synth/thumb9_{i % 8 + 1:02d}.wav"), T - SLIDE_LEAD, g - rng.uniform(6, 9))
    place(out, load(f"synth/grab2_{i % 12 + 1:02d}.wav"), T, g - GRAB_TRIM)
    place(out, load(f"synth/land1_{(i * 5) % 12 + 1:02d}.wav"), T + FLIGHT, g - rng.uniform(13, 16) + seat_gain_db(i))
    T += pace
with wave.open(f"synth/{name}.wav", "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(out[: int((T + 0.6) * SR)], -1, 1) * 32767).astype(np.int16).tobytes())
print("wrote", name)
