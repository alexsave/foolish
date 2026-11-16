"""Writes the two music scores (plain text note lists, player.h format) into /Users/alex/Music/foolish_cities_v5/alt_cello/scores/.
   python -I charts/music_scores.py"""
import math
SC = "/Users/alex/Music/foolish_cities_v5/alt_cello/scores/"

# ---- a) Bach BWV 1007 Prelude bars 1-4 (G major), quarter = 76
BACH_HEAD = """# J. S. Bach, Cello Suite No. 1 in G major, BWV 1007, Prelude, bars 1-4 (public domain). Quarter = 76, sixteenth = 0.19737 s.
# Notes: each bar is the 8-note figure played twice (bar1 G2 D3 B3 A3 B3 D3 B3 D3; bar2 G2 E3 C4 B3 C4 E3 C4 E3; bar3 G2 F#3 C4 B3 C4 F#3 C4 F#3; bar4 G2 G3 B3 A3 B3 G3 B3 G3).
# Confidence in the pitches: high (the opening is among the best known cello bars). Not in the score and added here: the last note of bar 4 is held 0.6 s.
# UNSURE / ASSUMED:
#   - fingering: G2, D3, A3 open; E3 F#3 G3 on D (1st-4th position, +2 +4 +5 semitones... D3=50: E3 +2, F#3 +4, G3 +5); B3 C4 on A (+2, +3). B3 is 1st position on A.
#   - bowing: the player has NO per-note slur (player.h slur is global, and a legato link reverses the bow at every linked note). Every note is
#     linked (gap 0.01 s < 0.15 s), so the bow changes direction at every sixteenth (legato changes, no per-beat bowing). A grouped bowing would need a per-note slur flag.
#   - vib is off (0) on every note, as a player plays running sixteenths. Open strings ring naturally.
#   - dynamics: vel 0.55 (mf) with a swell up each rising arpeggio (offsets below) and -0.06 on the last two notes of each bar.
# start dur midi vel string vib
"""
BACH = [[43, 50, 59, 57, 59, 50, 59, 50], [43, 52, 60, 59, 60, 52, 60, 52], [43, 54, 60, 59, 60, 54, 60, 54], [43, 55, 59, 57, 59, 55, 59, 55]]
BSTR = {43: "G", 50: "D", 57: "A", 59: "A", 60: "A", 52: "D", 54: "D", 55: "D"}
SWELL = [0.0, 0.01, 0.03, 0.045, 0.06, 0.03, 0.05, 0.02]  # a swell to the top B/C of each half-bar, then back


def write_bach():
    s16 = 60.0 / 76 / 4
    lines = [BACH_HEAD]
    t = 0.0
    for b, fig in enumerate(BACH):
        for rep in range(2):
            for i, m in enumerate(fig):
                v = 0.55 + SWELL[i]
                if rep == 1 and i >= 6: v -= 0.06
                if rep == 1 and i >= 6 and b < 3: pass
                last = b == 3 and rep == 1 and i == 7
                d = 0.6 if last else s16 - 0.01
                lines.append("%.4f %.4f %d %.3f %s 0\n" % (t, d, m, v, BSTR[m]))
                t += s16
    open(SC + "bach_bwv1007_bars1_4.txt", "w").write("".join(lines))


# ---- b) Bach Air (Orchestral Suite No. 3, BWV 1068), melody line, written from memory, transposed D major -> G major, down a twelfth (-19 semitones)
AIR_HEAD = """# J. S. Bach, Air from Orchestral Suite No. 3 in D, BWV 1068 (public domain), first violin melody, 8 bars, written from MEMORY, transposed from D major
# to G major and down 19 semitones (F#5 becomes B3), into the cello's middle register. Tempo quarter = 50 with rubato (starts drawn by a slow swing and phrase-end broadening).
# CONFIDENCE: LOW to MEDIUM. The shape (a long F#5, stepwise descent and rise, a leap to A5, a phrase closing on D5/C#5) I trust; the exact pitches and
# rhythm of bars 3-8 I do not (roughly 50 percent of the notes). Read it as "Air-like" for testing legato, vibrato and bow change, not as an edition.
# Vibrato: on (8 c) for notes of 1 beat or longer, off on shorter ones. Slide 1 (portamento) on two expressive shifts (the leap up to A5, E4 here, and the
# rising shift in bar 7). Strings automatic. vel shaped by phrase.
# start dur midi vel string vib depth rate slide
"""
# (D-major midi, beats, slide)
AIR = [(78, 3.0, 0), (76, 0.5, 0), (74, 0.5, 0),
       (73, 1.0, 0), (74, 1.0, 0), (76, 1.0, 0), (78, 1.0, 0),
       (81, 3.0, 1), (79, 0.5, 0), (78, 0.5, 0),
       (76, 1.0, 0), (74, 1.0, 0), (73, 1.0, 0), (74, 1.0, 0),
       (78, 2.0, 0), (76, 1.0, 0), (74, 1.0, 0),
       (73, 1.0, 0), (71, 1.0, 0), (73, 2.0, 0),
       (74, 1.0, 0), (76, 1.0, 0), (78, 1.0, 1), (81, 1.0, 0),
       (79, 1.0, 0), (78, 1.0, 0), (76, 2.0, 0)]
assert abs(sum(b for _, b, _ in AIR) - 32.0) < 1e-9


def write_air():
    beat0 = 60.0 / 50
    lines = [AIR_HEAD]
    starts, pos, t = [], 0.0, 0.0
    for m, b, sl in AIR:
        starts.append(t)
        ph = (pos % 16.0)
        slow = 1.0 + 0.04 * math.sin(2 * math.pi * pos / 7.0) + (0.10 * (ph - 12.0) / 4.0 if ph > 12.0 else 0.0)  # a swing and a broadening to each 4-bar phrase end
        t += b * beat0 * slow + 0.01 * math.sin(pos * 5.3)
        pos += b
    for i, (m, b, sl) in enumerate(AIR):
        t = starts[i]
        end = starts[i + 1] if i + 1 < len(AIR) else t + b * beat0 * 1.05
        d = end - t - (0.02 if i + 1 < len(AIR) else 0.0)
        p = (sum(bb for _, bb, _ in AIR[:i]) % 16.0) / 16.0
        vel = 0.40 + 0.28 * math.sin(math.pi * min(1.0, p * 1.05)) ** 1.3
        vib = 1 if b >= 1.0 else 0
        lines.append("%.4f %.4f %d %.3f - %d %s - %d\n" % (t, d, m - 19, vel, vib, "8" if vib else "-", sl))
    open(SC + "bach_air_g_major.txt", "w").write("".join(lines))


write_bach()
write_air()
