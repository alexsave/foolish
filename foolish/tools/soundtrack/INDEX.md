# City themes - index

All eight are original. The melodies and harmony are by a Fable agent working from the brief; I did the arranging, sound design, mixing and measuring.
Loudness target: -16 LUFS integrated, true peak no higher than -1.3 dBTP. It applies to every loop, every full version, and every playlist excerpt.
Every loop is seamless and sample-accurate. Each loop's start was compared with the render's own continuation: the residual is -120 dB or lower, and bit-identical for seven of the eight.
`playlist.wav` (2:52) plays 20 s of each loop in ladder order, with 1.5 s gaps.

Grades are my honest estimate. I cannot hear, so I judged from spectrograms, loudness, onset maps, chord and key detection, and the seam measurements, never by ear.

| # | City | Genre, tempo, key | Loop | Grade | One line |
|---|------|-------------------|------|-------|----------|
| 1 | Miami | tropical house, 100, A major | 76.8 s | B- | Pan-pipe and steel-pan hooks, Kygo-style kit and sidechain; synthetic pan pipe, a little mid-heavy |
| 2 | New York | 90s garage piano house, 122, F minor Dorian | 78.7 s | B- | 909 with choked hats, 62% swing, M1-recipe organ bass, piano hook; no vocal |
| 3 | Seoul | K-pop over a Jersey club beat, 140, E minor to F# minor | 82.3 s | C+ | Correct Jersey 5-kick, clearly minor (r 0.83), stop-hit dance break; no voices, a plain lead, bass-heavy |
| 4 | Madrid | salsa dura, 196 (2-3 clave), G minor / G major | 78.4 s | C | Correct clave, tumbao, guajeo, moña and cierre; GM percussion and brass sound toy-like, the horns play the coro |
| 5 | Vienna | Classical sacred, 12/8, D minor | 71.1 s | C+ | Real Mozart-style voice leading, suspensions and a chromatic climb; GM choir and strings |
| 6 | Warsaw | Chopin-style solo piano, cut time, G# minor / A-flat | 75.5 s | B- | 4 against 3, rubato, legato pedalling; my own additive piano, no soundboard model |
| 7 | St. Petersburg | Prokofiev-style ballet, 96, B-flat minor | 80.0 s | C | Ostinato with sideslips, striding theme, celesta middle; GM orchestra |
| 8 | Moscow | state-anthem style, 76, E-flat major | 75.8 s | C- | A stately TTBB refrain with bells; the GM choir is band-limited and dark, nothing like a massed male choir |

## Layout
- `engine/`: the shared C engine.
  - `gmrender.swift`: offline GM rendering over Apple's DLS.
  - `dsp.h`, `synth.h`, `voice.h`, `keys.h`: procedural instruments, including the additive piano.
  - `mix.h`, `render.h`: mixer, group buses, FDN reverb, limiter, BS.1770 loudness and the loop builder.
  - `anal.c`: the measurement tool.
  - `plot.py`, `zoom.py`: drawing only.
  - `playlist.c`.
- `N_city/`: `score.c` (the score, in C), `notes.md`, `out/full.wav`, `out/loop.wav` (48 kHz, 24-bit), `out/an_{full,loop}.png` and `.txt` (measurements).
- Re-render a city with `./build.sh N_city [full|loop|both]`.
- `melodies_fable.txt`: Fable's raw melody and harmony sheet.

## What changed after Alex's notes
- He called round 1 amateur, so I commissioned web research on professional practice for each genre. The specific techniques are listed in each notes.md with their sources.
- He called the lead vocals cheesy, so every synthesized voice is gone. Each city now leads with its own instrument.
- He asked for Seoul to be clearly minor. It now reads E minor at r 0.83 (it read E major before).
- He suggested Fable for the melodies: one Fable call wrote all eight cities' hooks and harmony.

## Biggest remaining weakness
Apart from the procedural dance synths and my piano, the sounds come from the 2 MB QuickTime-era GM set. The orchestral, choral and Latin themes (4, 5, 7, 8) are capped by those samples, whatever the arrangement does. A real sample library would need a download, which I did not do.
