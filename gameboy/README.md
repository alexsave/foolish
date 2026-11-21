# Durak for the Game Boy

Two-player Durak (you against one bot) for the original Game Boy (DMG), in C with GBDK-2020.
The bot is RANDOM: it plays a uniformly random legal move, drawn from the one seedable generator.

## Hypotheses (written before measuring)

- ROM: about 14 KB of the 32 KB ROM-only cart (code about 10 KB, as SDCC pulls in its 32-bit multiply and divide helpers; the binomial table 2.8 KB; tiles about 1.3 KB).
  No MBC needed.
  Falsifier: over 28 KB, which would force banking.
- RAM: about 4.6 KB of the 8 KB work RAM, most of it the cover-counting table (3.8 KB).
- Bot decision time (count the menu, draw, unrank, apply): 20 to 60 scanlines (2 to 7 ms) for a typical position, under 3 frames (50 ms) at worst.
  Falsifier: a typical move over one frame (154 lines, 16.7 ms).
