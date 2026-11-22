#!/usr/bin/env python3
# preview.py - one WAV to listen through: for each cue, the website's real
# take, then the Game Boy's synthesis (A), then its PCM (B) where there is
# one, a short gap between, a longer one between cues; then the short game
# in context (A, at the game's own frames). Writes BUILD/preview.wav (48 kHz
# mono) and BUILD/preview.txt, the listing with the time each clip starts.
#
#   python3 -I tools/preview.py BUILD
#
# Each clip is brought to the same loudness (loudest 50 ms at -20 dBFS, the
# peak held under -1 dBFS) so the three are compared by their sound, not
# their level: in the game the levels are the tables' own (DESIGN.md "Level").
# No randomness.
import sys, os
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyse import read_wav  # noqa: E402
from gen_tab import to_rate, write_wav  # noqa: E402

OUT_HZ = 48000
ORDER = ["deal", "deal_land", "turn", "flip", "lift", "place", "cover", "pass", "pickup", "into_hand", "sweep",
         "pile", "good", "hand_off", "out", "finish", "finish_miami", "finish_new_york", "finish_seoul",
         "finish_moscow", "burst", "fool"]
WEB = {"deal": "deal_tight_0"}


def norm(x):
    n = int(OUT_HZ * 0.05)
    l50 = np.sqrt(np.max(np.convolve(x * x, np.ones(n) / n, mode="valid"))) if len(x) > n else np.sqrt(np.mean(x * x))
    g = 10 ** (-20 / 20) / (l50 + 1e-12)
    pk = np.max(np.abs(x)) * g
    if pk > 10 ** (-1 / 20):
        g *= 10 ** (-1 / 20) / pk
    return x * g


def clip(path):
    sr, x = read_wav(path)
    return norm(to_rate(x, sr, OUT_HZ) if sr != OUT_HZ else x)


def main():
    b = sys.argv[1]
    gap, cue_gap = np.zeros(int(OUT_HZ * 0.35)), np.zeros(int(OUT_HZ * 1.0))
    parts, listing, t = [], [], 0.0

    def add(x, label):
        nonlocal t
        listing.append("%7.2f s  %s" % (t, label))
        parts.append(x)
        t += len(x) / OUT_HZ

    def silence(g):
        nonlocal t
        parts.append(g)
        t += len(g) / OUT_HZ

    for cue in ORDER:
        web = os.path.join(b, "web_wav", WEB.get(cue, cue + "_0") + ".wav")
        a = os.path.join(b, "gb_a", cue + "_0.wav")
        pcm = os.path.join(b, "gb_b", cue + "_0.wav")
        if os.path.exists(web):
            add(clip(web), "%s: the website's take (%s)" % (cue.upper(), os.path.basename(web)[:-4]))
            silence(gap)
        add(clip(a), "%s: Game Boy, synthesis (A)" % cue.upper())
        if os.path.exists(pcm):
            silence(gap)
            add(clip(pcm), "%s: Game Boy, 4-bit PCM at 8192 Hz (B)" % cue.upper())
        silence(cue_gap)
    sr, x = read_wav(os.path.join(b, "context.wav"))
    x = to_rate(x, sr, OUT_HZ)
    x = x * (10 ** (-1 / 20) / (np.max(np.abs(x)) + 1e-12))
    add(x, "IN CONTEXT: a deal, an attack of two, two covers, good, a pickup, a transfer, going out, Moscow's win, fireworks, the fool (A, the tables' own levels)")
    y = np.concatenate(parts)
    write_wav(os.path.join(b, "preview.wav"), y, OUT_HZ)
    with open(os.path.join(b, "preview.txt"), "w") as f:
        f.write("build/preview.wav, %.1f s: each cue's website take, then the Game Boy's versions\n" % (len(y) / OUT_HZ))
        f.write("(every clip at the same loudness; the in-context part at the tables' own levels)\n\n")
        f.write("\n".join(listing) + "\n")
    print("preview: %.1f s, %d clips" % (len(y) / OUT_HZ, len(listing)))


if __name__ == "__main__":
    main()
