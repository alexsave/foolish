#!/usr/bin/env python3
"""make_score_vib.py > score_vib.txt: per instrument an 8-bar phrase (four 4-beat notes, then a slow five-note line) twice, the first
section with `VIB PART: on`, the second with `VIB PART: off`, so the score-text switch is exercised end to end. 90 BPM."""
PH = {"CELLO": (["D3", "F3", "G3", "A3"], ["G3", "A3", "A#3", "C4", "D4"]),
      "HAEGEUM": (["C4", "E4", "G4", "C5"], ["C5", "D5", "E5", "F5", "G5"]),
      "TRUMPET": (["D4", "F4", "A#4", "D5"], ["D5", "C5", "A#4", "G4", "F4"])}
print("TEMPO 90\n")
for part, (longs, line) in PH.items():
    bars = ["%s:4" % n for n in longs]
    # the five-note line, lengths 4, 3, 3, 3, 3 beats (notes held across bar lines with ~): bar sums 4 each
    b = ["%s:4" % line[0], "%s:3~3 %s:1~3" % (line[1], line[2]), "r:2 %s:2~3" % line[3], "r:1 %s:3" % line[4]]
    for state in ("on", "off"):
        print("SECTION %s vibrato %s BARS 8 KEY C" % (part.lower(), state))
        print("%s: %s" % (part, " | ".join(bars + b)))
        print("VIB %s: %s\n" % (part, state))
