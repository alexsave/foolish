# 5 Vienna - an angry requiem with the piano at its centre (round 4)

Six parts, 142 bars, 6:14.
The suite is its own seamless loop: the last bar is A7, hammered, and its resolution is Part I bar 1 (the piano's pianissimo menace). Loudness -16 LUFS integrated; the key estimate is D minor.
Files:
- `out/full.wav` (the suite, also the loop), `out/an_loop.png` / `.txt`, `out/sections.txt`.
- `score.txt` (Fable's round 4 revision), `score.c`.
- `fable_suite_v2.md` (the composer's full notes, with every Beethoven figure located), `r3a_score.txt` (the round 3 score).
- Round 2 is kept in `r2/`.

## Plan

| Part | Bars | Tempo | Key | Face | Arc |
|---|---|---|---|---|---|
| I Dies irae | 1-32 | 132 | D minor | the wrath | the piano alone, pianissimo (the Appassionata opening: a broken chord up the keyboard, the wrath motif whispered in octaves) -> the tutti explodes at bar 3 -> second statement with the Neapolitan and the C# diminished seventh, the choir hammering off the beat against piano and strings on it -> the piano alone, forte, hammering repeated-note octaves over Pathetique tremolo basses -> unison hammering -> cadenza runs -> D major, then D7 |
| II Rex tremendae | 33-48 | 60 | G minor | the throne | Pathetique grave: double-dotted chords over tremolo octave basses, organ and trombones hammering the choir -> it cuts dead; two bars of the piano alone, piano -> the Neapolitan A-flat at fff -> a hocketed diminished seventh -> D7 with the piano and violins hammering A |
| III Fugato | 49-78 | 116 | D minor | the pursuit | the Moonlight quote, then four choral entries and a fifth for the piano alone -> an episode with Alberti sixteenths and sighs halving in length -> subject in F, inverted -> the chromatic bass climb -> stretto -> dominant pedal with diminished-seventh whirls -> unison A |
| IV Recordare | 79-98 | 76 | F major | the plea (the Romanze of K. 466) | the piano alone sings the plea over an Alberti bass, with clarinets in canon -> the choir takes it up -> the wrath breaks in for three bars and stops dead -> the piano tries the plea again, shaken -> the walk back to D minor through the Neapolitan |
| V Lacrimosa | 99-116 | 54 | D minor | the lament | 12/8 lilt with the piano's triplet arpeggios -> the choir from bar 3 -> the second swell reharmonised a third higher -> the hammered diminished seventh -> the piano runs down two octaves over the timpani roll |
| VI Dies irae returns, Amen | 117-142 | 132 | D minor | the opening returns | Part I's tutti note for note -> the motif inverted with the piano hammering the chords -> an Amen stretto with the piano doubling each entry -> Neapolitan, dominant, tonic shouts -> the unison hammering sequenced up by semitones -> a two-bar piano cadenza -> plagal Amens -> A7 into bar 1 |

**Motifs**
- **Motif A**, the wrath (A D D D C# D F):
  - whispered by the piano alone, then hammered by everyone (I);
  - the skeleton of the shouts (II), the fugue subject and the piano's entry (III);
  - the plea's head, soft and in major, broken into by its own fortissimo form (IV);
  - the Lacrimosa's upbeat (V);
  - all of VI, inverted, and hammered in the cadenza.
- **Motif B**, the sigh (falling half steps in pairs): the violas under the choir (I), the sopranos' halving sighs (III), the plea (IV), the second half of every Lacrimosa phrase (V), the chromatic slide (VI).

**Key journey**: D minor -> G minor (with the Neapolitan A-flat) -> D minor through A minor, C, F, B-flat and the chromatic climb -> F major, broken into by D minor -> D minor with the Neapolitan E-flat -> D minor, ending on A7.

**Hand-offs**
| Bars | Device |
|---|---|
| 30-32 -> 33 | D major and D7 become V of G minor; the choir, brass and timpani hammer D7 off the beat while the piano's two-octave run falls through to the D that Part II opens on |
| 46-48 -> 49 | the same bar twice with only the last beat changed; D7 with the piano and violins hammering A; the cellos' held D is the subject's first note |
| 78 -> 79 | unison A in every octave, the piano's tremolo and the timpani roll dying; Part IV opens on F major with that A |
| 93-98 -> 99 | the plea turns to D minor through the Neapolitan; the piano's tremolo A carries into the Lacrimosa |
| 116 -> 117 | the hammered diminished seventh, the piano's two-octave run over the roll, the dotted upbeat |
| 142 -> 1 | A7 hammered; the loop lands on the piano's pianissimo menace |

**Quote (for the record)**: Beethoven, Piano Sonata Op. 27 No. 2 ("Moonlight"), third movement, bar 1, the rising two-octave arpeggio, transposed to D minor. It is in the piano's right hand with the first violins at Part III bar 1 (bar 49), and bar 2 answers with the sonata's two sforzando chords. Re-performed with our samples; no original audio is used.

## Alex's notes and what changed
- **"Where's the piano? Is this Vienna or Tchaikovsky?"**: Fable rewrote the suite as a concerto (Mozart K. 466 crossed with Beethoven).
  - The Salamander grand is in all six parts: alone, against the choir, driving under the tutti, and running at every hand-off.
  - Its Beethoven figures (Moonlight arpeggios, Pathetique grave chords and tremolo basses, Appassionata hammering and pianissimo menace) are located bar by bar in `fable_suite_v2.md`.
- **"Not angry enough"**: more sforzandi, hocketed hammer blows (choir off the beat, strings and piano on it), and a Neapolitan or diminished seventh in every part. Dynamics are mostly f-fff, and three deliberate drops set up the next attack. The plea is interrupted by the wrath.
- **"The strings are the most synthy... almost played in reverse"**: two causes, both fixed in the engine for every city.
  - The VSCO section sustains carry a 0.5-1 s recorded bow swell. At 132 BPM the notes never reached full level and then stopped. The sampler now measures each sample's swell and skips it according to note length.
  - Violins above D6 were the section samples stretched up to 8 semitones. They now play on three VSCO solo violins with vibrato, recorded up to C7.
  - Also, 12 cents of vibrato at 5.6 Hz is added on top of the recorded vibrato, and every short note (an eighth or less) plays the spiccato samples.
- **"Something sounds almost like an electric guitar around 2:10"**: the fugato's exposed entries. The formant layer of the choir had a bright tilt and a singer's-formant boost, which buzzed. In the 126-134 s stem window, the tenor formant layer was the loudest thing at 2-4 kHz.
  - The layer is now 9 dB under the GM layer, with a -3 dB/octave tilt and breath noise.
  - Its harmonic ceiling moved from 7 kHz to 9.5 kHz, so no static "low-pass ceiling" shows on the spectrogram (the blind reviewer's other item).
- **"The vocal aahs are done well"**: they are kept (GM choir plus formant ensemble).
- **Basses an octave high**: the VSCO contrabass manifests were an octave high, so the basses doubled the cellos. Fixed.
- **Organ pedal**: the lowest note of each organ chord is doubled an octave down (a 16' stop), which puts the low D under the wrath.
- **Breath**: an 8% ritardando into every section end (`sec_rit`).
- **Texture**: mono below 120 Hz (now a true high-pass on the side signal), a light tape stage, room tone, an 18.5 kHz roll-off.

## Measurements
To be filled from the final render.
