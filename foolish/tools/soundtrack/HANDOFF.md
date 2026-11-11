# Soundtrack handoff (paused 2026-11-02)

Alex paused the city themes to return to sound effects.
No render is running. Every `out/full.wav` is complete, and every one is a seamless loop (the seam residual is -143 dB or better), at -16 LUFS integrated.
Each city's `notes.md` holds its plan, transitions, quote, measurements, iteration log and weaknesses. The round 4 sections are appended at the end.

## State of each city

| City | Current full.wav | State |
|---|---|---|
| 1 Miami | round 4 rebuild, 6:33 | The approved "2 a.m." direction: F minor melodic / deep house with a Cuban percussion layer, and one developing 4-note motif. The score is by Fable. Alex's latest note: "doesn't sound like shit, but it does sound a bit too classical, not quite EDM". The next pass should push the production toward EDM: thinner orchestral strings, synth leads and stabs carrying the motif, harder kick/clap/hat programming, real drops and builds with risers and filter automation, and sidechain to taste. |
| 2 New York | round 4, 6:44 | Part 1 rewritten (Fable). An 808 under every beat. Scratches now work the playing record through the turntable stage. Near-mono, matched to the Nas reference for spectrum and width. The reviewer's 18% clash-rate item is unchecked. |
| 3 Seoul | round 4b | The "wiip" was the SQUEAK part (a 1.9-2.8 kHz chirp); it is muted. Orchestra trimmed behind synth layers. Foundation and density matched to LE SSERAFIM "Eve". Open: the reviewer's 8-bar lead theme rewrite and its 20% semitone-clash check (key clarity is low). |
| 4 Madrid | round 4 score with the round 4b engine | The latest render (12:40) adds a sine sub under the baby bass, chord rolls on piano, tres and vibes, the true mono-lows fix, a tape stage and a low shelf. It was rendered and seam-checked but not analysed or reported; it is a mix change only. Open: the reviewer's "tune that returns 3+ times (AABA)", swing and ornaments; it read the suite as a "fantasy tavern". |
| 5 Vienna | round 4 score (Fable: the piano at the centre, angrier) | The latest render (12:40) has the organ 16' pedal, the true mono-lows fix and section ritardandi. It was rendered and seam-checked but not reported to main. Its `notes.md` is written except the Measurements section. Open: the reviewer's chord pace (one change every 8-20 s, not every 3 s), a clear arc, composed fades instead of hard dips. LRA is 5.6 LU against the reviewer's 10-15. |
| 6 Warsaw | round 4b | Elegy extended (Fable). Phrase rubato, melody lead, chord rolls, prepared tempo change. Reported. The 12:40 render only adds the true mono-lows fix. |
| 7 St. Petersburg | round 4 plus engine | Alex: "this is art". High strings and trumpets were fixed. The 12:40 render adds ritardandi into slower numbers and a short accelerando into the street fight ("TEMPO x RAMP 4 AT -4"), the engine vibrato, tape/room and the true mono-lows fix. These are not yet reported. Change it only where its rhythm is genuinely lost (the reviewer found little pulse). |
| 8 Moscow | round 4b | Fable revision. Accelerando into the march, presence and sub against the USSR anthem recording. Open: the reviewer's brass theme of 4-8 bars over the ostinato and moving beyond i-V. |

## Still open
- **Vienna rework**:
  - verify Alex's four points on the 12:40 render (piano central, angrier, strings no longer synthy or reversed, the 2:10 "electric guitar" gone);
  - the reviewer's chord pace and arc;
  - fill in the Measurements section of `notes.md`, then report.
- **Composition passes** (Fable ran out of credits; use Fable when it is back, otherwise Opus, per CLAUDE.md):
  - Seoul: an 8-bar lead theme; check the 20% semitone clashes for wrong notes.
  - Moscow: a 4-8 bar brass theme over the ostinato, quiet near 1:37 and full at the 4:13 climax; harmony beyond i-V (i-VI-III-VII, chromatic mediants).
  - Madrid: a real tune (8+8, AABA) returning at least 3 times; salsa signifiers that land.
  - St. Petersburg: only where the rhythm is lost (articulate ostinati).
  - All: the reviewer's top item. A main motif stated, developed and returning at least 3 times, with self-similarity at returns of 0.9 or higher. Miami (0.90), NY (0.96), Vienna (0.95), Madrid (0.92) and Seoul (0.98) already pass on the measure itself.
- **Miami**: "a bit too classical, not quite EDM" (see the table).
- **Reviewer items not done**:
  - LRA targets for cinematic tracks (Vienna 5.6, Moscow 7.7 against 10-15);
  - Miami's low-end width (side/mid low -21 dB against the references' -10 to -14);
  - Moscow's air (8 dB under the anthem).
- **Admin**: INDEX.md (still round 2), playlist.wav (rebuild with `engine/playlist`, 20 s per city in ladder order), and LICENSES.md's disk total (samples about 6.4 GB).

## What changed in the engine this session (all cities share it)
- **`sampler.h`**:
  - per-sample bow-swell measurement and length-aware skipping (sp12, set by `o_smp` for looped sustains);
  - added vibrato (sp13 depth in cents, sp14 rate; strings get 12 cents at 5.6 Hz in `o_smp`, second players 7);
  - +-2 cents and +-0.6 dB variation per note.
- **`orch.h`**:
  - `o_hi` sends violins above D6 to three VSCO solo violins (manifest `svln_vib`);
  - `o_roll` rolls chords;
  - `o_hall` sets mono lows, tape, room tone, a top roll-off and a sub shelf.
- **`render.h`**:
  - the turntable stage (`turntable()`: baby, chirp, transformer, stutter, brake, backspin on group buses);
  - anti-aliased (ADAA) saturation for track and group drive;
  - a crusher reconstruction filter;
  - master `tape`, `room_db` (high-passed), `top_hz` and `side`;
  - true mono lows (high-passed side);
  - step-vs-ramp filter automation with loop-safe tie ordering;
  - MAXTR 96 with a guard.
- **`scoretxt.h`**:
  - a bar-anchored DYN parser;
  - "TEMPO x RAMP n AT -m" (ramps before a boundary);
  - `sec_rit` (cadence and phrase ritardandi);
  - a lone token with no length means a whole bar;
  - `sec_lp` step semantics.
- **`keys.h`**: the e-piano tremolo is locked to the loop (it had broken NY's seam).
- **`synth.h`**: ADAA in the supersaw drive.
- **Manifests**:
  - the contrabass manifests (`cb_*`) moved up an octave (they were an octave high);
  - new `svln_vib` and `latin_*` (VCSL congas, bongos, bells, claves, guiro by stroke, built by `engine/mkmulti.py`).
- **References**: the Musopen Figaro overture premix (CC0, measurement only) is in `samples/ref_figaro/` and listed in LICENSES.md. Alex's six reference mp3s are in `../refs/` (measurement only).

## How to resume
1. **Render**: `cd soundtrack/<city> && clang -O2 -w -o score score.c && ENGINE=../engine nice ./score loop out` (5-7 min each; one at a time).
   - For several cities in a row: `../wtmp/queue.sh <dirs...>`, which logs to `../wtmp/queue.log`.
   - Then `cp out/loop.wav out/full.wav` once the seam line reads -140 dB or better.
2. **Seam bugs**: debug with `SEAMDBG=1` (per-track residuals) and `SOLO=<track>` (renders one track in seconds).
3. **Measure**:
   - `../engine/anal out/loop.wav out/an_loop $(cat anal_args.txt) --sec "$(cat sections_loop.txt)" --loop` and `python3 -I ../engine/plot.py out/an_loop "<title>" "$(cat sections_loop.txt)"`;
   - `../engine/fullness out/loop.wav`;
   - the reference comparison `python3 -I ../refwork/refcmp.py out.png ../refs/<ref>.wav out/loop.wav`;
   - the blind reviewer's gates: `cd ../wtmp/gates && ./dec.sh <name> <wav> && python3 -I levels.py <name>; python3 -I analyze.py <name>`.
4. **Composition**:
   - Fable's original and revised scores and notes are in each city's `fable_suite*.md`.
   - A revision brief should restate the strict score format (the briefs in this session's transcript are the template). It should also restate Alex's two rules: transition-quality writing throughout, and parts long enough to breathe.
