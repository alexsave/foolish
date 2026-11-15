# Audio analysis tools

DSP measurement tools for decoded audio (numpy, scipy and matplotlib only; ffmpeg for decoding and loudness).
Everything they print is a measurement, not a listening verdict.
Run everything as `python3 -I` (Python 3.15).
Every input and output path is a command-line argument; no JSON is read or written.

## Tools

`lib.py` is the shared DSP library.
It decodes through ffmpeg, computes the STFT, onset envelope, windowed tempo and tempo map, global tempo, beat grid, chroma and key, chords per bar, bass and melody salience, HPSS, sections by checkerboard novelty, band shares, BS.1770 loudness, the ending class and a voice heuristic.
It owns the only random source, `rng(seed=SEED)`, with `SEED` the single named constant.

`analyze_clip.py` measures one audio file.
It writes a Markdown report (summary numbers, tempo map, beat grid, sections, per-bar chord, bass and melody tables for the windows you name, voice heuristic), a spectrogram PNG under 400 kB and one summary TSV row.
Example: `python3 -I analyze_clip.py song.mp3 --out reports --prompt song.txt --window 30 70 keep "good drop" --window 90 end drop ""`.

`analyze_batch.py` runs the same measurement over a list file of audio paths.
An optional windows TSV (KEY, START, END, VERDICT, NOTE) selects which files also get a report and PNG.
It writes `summary.tsv` (one row per file, rewritten after every file) and the reports in one sub folder per parent folder of the audio.
Example: `python3 -I analyze_batch.py files.txt --out reports --windows windows.tsv`.

`measure_render.py` measures one engine render (a wav plus its sections.txt).
It writes the Markdown report (headline loudness, the last 12 seconds, the sections verbatim, the tempo map, boundary level tables around chosen section starts, a tail table, optional band-share and onset-density comparisons), the PNG and a per-second level TSV.
With `--old` it puts the same windows of an older render next to the new ones.
Example: `python3 -I measure_render.py --new out/full.wav --sections out/sections.txt --out reports --old old.wav --boundary "A -> B" "B" 122 162 --tail "Return to end" "V Return"`.

`test_analysis.py` holds the tests (below).
Run it with `python3 -I test_analysis.py`; it takes a few seconds and exits 0 or 1.

## Tests

- `test_tempo_and_key`: a 40 s synthetic loop at 97 BPM in A minor and one at 184 BPM in G minor; the BPM must be within 1 percent and the key must be right.
- `test_melody_and_bass`: a synthetic tune with known notes on every half beat over a bass line A1, D2, E2, A1; at least 80 percent of the melody notes and of the bass notes in the per-bar table must be in the true note sets.
- `test_vibrato`: an 8 s harmonic tone; the vibrato term must be above 0.9 for a 6 Hz plus or minus 40 cent vibrato and exactly 0 for a steady tone and for a 1.5 Hz wobble.
- `test_rng_deterministic`: `rng()` gives the same draws every time and different seeds give different draws.
- `test_no_stray_random`: greps every `.py` file in this folder for random and time based sources (numpy and stdlib random, default generators, rand calls, time based seeds, dynamic imports) and fails on any hit outside the one line of lib.py marked `RNG-SOURCE`; it also requires exactly one such marked line.

## Mutation checks

Each test was broken on purpose by editing the code it covers, run, and the code restored byte for byte (verified with cmp) before the next one.
The run after the last restore passed all five tests.

- Mutation: Halve the hop in the lag-to-BPM conversion (`fse = fs` becomes `fse = 2*fs` in `acf_peaks`).
  Failed test: `test_tempo_and_key`.
  Assertion text: "184 BPM G minor: measured 122.66 BPM, true 184 (more than 1% off)".
- Mutation: Key estimator picks the worst-correlating key (sort order of `key_est` reversed).
  Failed test: `test_tempo_and_key`.
  Assertion text: "97 BPM A minor: measured key Bb minor, true A minor".
- Mutation: Force the melody tuning offset to +100 cents (`bar_table` candidate frequencies).
  Failed test: `test_melody_and_bass`.
  Assertion text: "only 41% of melody notes are in the true note set (C#5 C#5 E5 E5 G#5 ...)".
- Mutation: Force the bass tuning offset to +100 cents (`bar_table` bass candidates).
  Failed test: `test_melody_and_bass`.
  Assertion text: "only 0% of bass notes are in the true set (C#2 C#2 ... D#2 ...)".
- Mutation: Zero the vibrato term (`vib = 0.` in `voice_heuristic`).
  Failed test: `test_vibrato`.
  Assertion text: "vibrato term of a 6 Hz +-40 cent tone is 0.00, expected above 0.9".
- Mutation: Vibrato term always 1 (`s_v = 1.0`).
  Failed test: `test_vibrato`.
  Assertion text: "vibrato term of a steady tone is 1.00, expected 0".
- Mutation: Stray `np.random.rand()` call in the `to22` helper of lib.py.
  Failed test: `test_no_stray_random`.
  Assertion text: "stray random or time sources: lib.py:47: _ = np.random.rand()".
- Mutation: Add `random` to the import line of analyze_batch.py.
  Failed test: `test_no_stray_random`.
  Assertion text: "stray random or time sources: analyze_batch.py:18: import sys, os, argparse, traceback, random".
- Mutation: A `__import__('time').time()` call in measure_render.py.
  Failed test: `test_no_stray_random`.
  Assertion text: "stray random or time sources: measure_render.py:50: _ = __import__('time').time()".
- Mutation: `rng()` ignores its seed (default generator created with seed None).
  Failed test: `test_rng_deterministic`.
  Assertion text: "rng() with the default seed is not deterministic".

## Refactor check against the scratch scripts

`analyze_clip.py` on a 121 s K-pop clip with its prompt and three windows reproduced the earlier report exactly.
The only difference in the Markdown is the pointer line to the method README, which no longer holds an absolute path.
The summary row and header, the section boundaries (11.8, 38.8, 45.2, 71.2, 77.8, 90.2, 108.2, 115.8 s) and the spectrogram PNG are byte identical.
`measure_render.py` reproduced the numbers and the per-second level TSV of the original per-song script byte for byte on a 472 s render with an old comparison; only labels and a few generalised sentences differ.
`hop_err`, the validation-only knob of the first scripts, is gone; the lag-to-BPM mutation above edits the code instead.

## Notes

The `measure_render.py` options are generic; the song specific names of the first script (section names, old time windows) are now arguments.
`analyze_batch.py` resolves a file's prompt from `<stem>.txt` next to it, `prompts/<stem>.txt`, or `../prompts/<parent>/<stem>.txt`.
With no `--window`, `analyze_clip.py` makes its per-bar table over the whole file (the first scripts only made tables for named windows).

# Method and limits

## Decoding

Every file is decoded with ffmpeg to 44.1 kHz stereo float (other sample rates are resampled by ffmpeg).
The 44.1 kHz mono mix is also decimated by 2 (scipy resample_poly) to 22.05 kHz for the pitch, tempo and chroma work.
Loudness, true peak and loudness range come from the original file through ffmpeg `ebur128=peak=true` (the Summary block).


## Measurements and their failure modes

- Loudness curve and "LUFS-like" section levels: BS.1770 K-weighting filters (shelf and high-pass, recomputed for the sample rate) on the 44.1 kHz mono mix, mean-square over the span, -0.691 offset.
  Short-term curve uses a 3 s window.
  Mono sum instead of the stereo channel sum, no gating, so it differs from the ffmpeg integrated value by a fraction of a dB to a couple of dB.
  Failure mode: very quiet spans are not gated.
- Band energy shares: 2048-point STFT at 44.1 kHz, power summed in sub (<60 Hz), bass (60-250), lowmid (250-1000), mid (1000-4000), high (4000-10000), air (>10000), in dB relative to the total power.
  Failure mode: mp3 low-pass filters empty the air band, so air reflects the codec as much as the music.
  The 21.5 Hz bin width makes the sub band only 2 to 3 bins wide.
- Spectral centroid: power-weighted mean frequency over the whole file (not the mean of per-frame centroids), so loud passages dominate.
- Stereo side/mid: 10 log10 of the energy of (L-R)/2 over the energy of (L+R)/2.
  A value near -30 dB or below means near-mono.
- Onset envelope: 1024-point STFT, hop 128 at 22.05 kHz (172 Hz), 40 mel bands (30-10000 Hz), log magnitude in dB with an 80 dB floor, positive difference at lag 4 frames, mean over bands.
- Onset density: peaks of the Gaussian-smoothed onset envelope (sigma 1.5 frames) with prominence of at least 0.5 standard deviations and 50 ms minimum spacing, per second.
  It depends on that threshold; compare files, do not read it as a note count.
- Percussive share: HPSS by median filtering a 128 band mel magnitude spectrogram (31 frames across time for the harmonic estimate, 13 bands across frequency for the percussive estimate), soft mask with power 2, share of energy assigned to the percussive part.
  Failure mode: bass notes and sharp piano or pluck attacks count as percussive, sustained pads count as harmonic.
- Tempo per window: autocorrelation of the onset envelope in 8 s windows with a 4 s hop, lags for 50-210 BPM, a log-normal prior (centre 120 BPM, sigma 1.3 octaves), parabolic peak interpolation, then the top 5 peaks per window are chosen by Viterbi with 0.5 score units of penalty per octave of change between neighbouring windows.
  Both the smoothed pick and the raw strongest peak are printed in each md.
  Pulse strength is the raw normalised autocorrelation at the chosen lag (0 to 1); below 0.15 a window is marked weak and below 0.20 for a whole file the md says so.
- Global tempo: peak of the mean of all window autocorrelations, refined by a fine Fourier comb search of plus or minus 4 percent when the windowed tempos are stable (std below 2 percent), otherwise the median of the windowed tempos.
  The second candidate is the next peak at least 6 percent away.
  If the slower of two candidates in a 2:1 relation has an odd/even onset-strength ratio of at least 0.85 (the strength of the envelope halfway between the beats divided by that on the beats, using the whole file), the estimator picks the faster one and says so in the md.
- Tempo stability: std in BPM of the windowed tempos after folding each by a factor of 2 to the octave of the global tempo.
  A file with a real tempo change or a meter flip has a large value.
- Tempo steps: the md lists every step above 3 percent between neighbouring windows, with a note when the ratio looks like 2, 1/2, 3/2, 2/3, 4/3 or 3/4, which is what an estimator flip looks like, and which a real change of beat level can also look like.
  Windows with pulse strength below 0.15 are skipped.
- Tempo against the prompt: for ratios of 2 or 1/2 the md prints the odd/even onset-strength ratio at the slower grid (above about 0.7 the faster level has real onsets between the slow beats, below about 0.5 it does not).
  For other ratios (1.5, 4/3, 3/4, 2/3) the md states the ratio and says the test does not resolve it.
- Beats: dynamic-programming beat tracker (Ellis style, tightness 100) on the onset envelope at 86 Hz plus 0.7 times its low-band version, with the local period taken from the tempo track (octave flips smaller than 8 percent are folded to the previous window).
  The md lists every 4th beat time.
  Failure mode: the grid can lock to off-beats (hi-hats between kicks), to a half-time or double-time level, or to a slightly wrong period (the validation run shows a 0.5 percent period error from the 11.6 ms frame step).
- Bars and downbeat: 3 or 4 beats per bar chosen from the accent profile of the beat-synchronous onset strength (4 unless 3 leads by more than 1 z-unit).
  The z value and its lead over the alternative are printed; below about 1.5 treat bar boundaries as a guess.
  Failure mode: any meter other than 3 or 4 (6/8, 12/8, 5) is forced into 3 or 4.
  The "first downbeat-like strong beat" is the first beat on the chosen phase whose accent is at least the median of that phase.
- Tuning: histogram of spectral peak deviations from 12-tone equal temperament (interpolated peaks 100-2000 Hz, circular mean), reported in cents; set to 0 and flagged when the concentration is below 0.2.
  Failure mode: noisy or percussive material, bends and detuned synths.
- Chroma and key: 4096-point STFT (hop 512, 22.05 kHz), magnitudes square-rooted, bins 80-3500 Hz mapped to 12 pitch classes after the tuning correction, frames L2-normalised and weighted by their level.
  Key is the best of 24 Krumhansl-Schmuckler profile correlations.
  Key confidence is the correlation gap to the runner-up key.
  A gap below 0.05 means the key is not established by this method.
  Failure modes: relative major/minor confusion, modal or modulating music, bass-heavy mixes (harmonics of the bass note land in other pitch classes), and a single key for a whole file that modulates.
- Key against the prompt: match, relative (relative major/minor), parallel, fifth-related or differs, compared with every key named in the prompt text.
  Prompts with no key leave the cell blank.
- Sections: features every 0.5 s (chroma plus 6 band energies in dB, each z-scored and block-weighted), cosine self-similarity, Gaussian-tapered checkerboard kernel (half width 6 s for files over 40 s, 3 s otherwise), peaks above mean plus 0.5 std with 6 s (3 s) minimum spacing, none within 2.5 s of the ends.
  The section table gives for each section the LUFS-like level, median tempo, band shares, onset density and percussive share.
  Failure modes: a smooth build is not detected, a repeated section is detected only at its edges, boundaries can sit one or two seconds from the audible change.
- Chord per bar: mean of the chroma frames in the bar compared with 24 major and minor triad templates by cosine.
  Reported as the best chord with score and margin to the second best.
  Marked "unclear" with the best guess in brackets when the margin is below 0.03 or the score below 0.55.
  When a 4-beat bar has two different confident chords in its halves they are shown as (first|second).
  Failure modes: extended and seventh chords, inversions, single-note melodies over a pedal, wrong bar boundaries.
- Bass note: a 5.5 kHz decimated signal, 4096-point STFT (hop 256), harmonic-sum salience (4 harmonics, weight 0.85 per harmonic) over notes E1 to G3 (41-196 Hz), averaged per beat and per bar.
  A dash means the level is below 5 percent of the loudest bass frame in the window.
  Failure modes: octave errors, missing fundamental in small speakers is not relevant here but a strong second harmonic can win, kick drum pitch can be read as a bass note.
- Melody: harmonic part of the 4096-point spectrogram (median filtering across 17 frames against 17 bins, soft mask), spectral whitening, harmonic-sum salience (5 harmonics, 0.8 decay) for semitone candidates 196-2637 Hz, averaged per half beat.
  The octave is the strongest candidate's octave, which is a guess.
  The margin is the relative gap to the best candidate at least 2 semitones away; a question mark follows a note when that margin is below 0.1.
  A dash means the 200-2500 Hz energy is below 8 percent of the 90th percentile of the window.
  One note per half beat means fast runs are undersampled and held notes repeat.
  Polyphonic textures give a single dominant pitch, not the melody a listener would follow.
- Possible voice: per 2 s block a score of 0 to 1 from four terms (energy share 300-3400 Hz over the full spectrum, harmonic share of that band, vibrato of the strongest 150-1000 Hz pitch, number of broad formant-like peaks after 120 Hz smoothing).
  The md lists ranges at or above the file's 90th percentile score clipped to 0.35-0.50, with the four terms of the peak block.
  This is an unverified heuristic that cannot tell a choir from strings, a flute or a lead synth.
  The vibrato term needs a stable single pitch for at least 0.5 s with a confidence gate, so in dense mixes it is almost always 0.
- Ending: level in 100 ms steps over the last seconds.
  Hard cut when the final 100 ms is less than 12 dB below the 75th percentile level of the preceding seconds, or when the level stays within 6 dB of that reference until less than 0.5 s before the end.
  Otherwise ring-out when there is no onset energy in the last 2 s above 35 percent of the preceding 90th percentile (the level decays after the last notes), else fade (notes continue while the level goes down).
  The md prints the drop in dB and how many seconds before the end the level fell more than 6 dB below the reference.
  Failure mode: a fade and a ring-out of reverb tails are told apart only by onset presence in the last 2 s.
  The last-second level is the RMS of the final 1 s in dBFS (-120 means digital silence).
- Spectrogram png: 4096-point FFT at 44.1 kHz, up to 1600 columns, interpolated to 256 log-spaced rows 30 Hz to 16 kHz, 90 dB dynamic range below the file maximum.
  Section boundaries are dashed lines, the windows given with --window or in the windows TSV are cyan bars at the top and cyan shading in the loudness panel.
  All png files are under 400 kB.

## Could not measure

- Whether anything is a human voice, a choir or an instrument: only the heuristic above exists, and no vocal separation is possible without installing a model.
- Instrument identity (trumpet, cello, pan flute, guitar, scratching, etc.): no classifier was available, so the md files carry no instrument labels except what the prompt says.
- Real downbeats: bar boundaries come from beat accents only, with no knowledge of the score.
- Key per section and key changes inside a file: only one global key is reported.
- Melody of dense polyphony, chord inversions and extensions, exact octave of melody notes.
- Audio above 11 kHz in the section tables (band shares come from the 44.1 kHz STFT, but the onset, chroma and HPSS work is at 22.05 kHz).
- The prompt BPM and key come from regular expressions on the prompt text (a number before BPM, a note name before major or minor); a prompt that states the tempo or key another way is blank or partial.

