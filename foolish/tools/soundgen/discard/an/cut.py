"""Cut Alex's discard recordings (2026-11-07: IMG_3046, IMG_3047, IMG_3048, extracted to mono 48 kHz) into reps.
Each video: the hand sweeps the table's cards together in 4-5 scraping strokes, lifts the pile and turns it over (two
knocks on the table about 45 ms apart), then sets it softly on the discard pile. Times are from the spectrograms and the
frames (the pile is lifted at 3.95-4.1 s in IMG_3047 and lands face down on the pile at 5.75 s).
Usage: cut.py DIR_WITH_d3046.wav..."""
import sys, wave, numpy as np
SR = 48000; SRC = sys.argv[1]
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16)
def save(p, x):
    with wave.open(p, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes(x.astype(np.int16).tobytes())
SCRAPES = {3046: [(0.59, 1.53), (1.69, 2.02), (2.14, 2.55), (2.62, 3.82)],
           3047: [(1.01, 1.54), (1.72, 2.22), (2.42, 2.68), (2.80, 3.26), (3.31, 3.78)],
           3048: [(1.18, 1.78), (2.03, 2.72), (2.90, 3.25), (3.36, 3.67), (3.82, 4.12)]}
LIFTS = {3047: (3.70, 4.30), 3048: (4.10, 4.70)}               # the first knock lands at 0.15-0.2 s into the cut
PLACES = {3047: (5.60, 6.20), 3046: (6.05, 6.65)}
ROOM = {3047: (4.40, 5.50)}
k = 0
for v, segs in SCRAPES.items():
    x = load(f"{SRC}/d{v}.wav")
    for a, b in segs:
        k += 1; save(f"parts/scrape_{k}.wav", x[int((a - 0.05) * SR): int((b + 0.03) * SR)])
for i, (v, (a, b)) in enumerate(LIFTS.items(), 1): save(f"parts/lift_{i}.wav", load(f"{SRC}/d{v}.wav")[int(a * SR): int(b * SR)])
for i, (v, (a, b)) in enumerate(PLACES.items(), 1): save(f"parts/place_{i}.wav", load(f"{SRC}/d{v}.wav")[int(a * SR): int(b * SR)])
for v, (a, b) in ROOM.items(): save("parts/room.wav", load(f"{SRC}/d{v}.wav")[int(a * SR): int(b * SR)])
print(k, "scrapes,", len(LIFTS), "lifts,", len(PLACES), "placements, room")
