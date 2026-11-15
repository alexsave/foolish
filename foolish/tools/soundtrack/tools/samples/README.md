# VocalSet choir extraction

VocalSet (Wilkins, Seetharaman, Wahl, Pardo, ISMIR 2018), https://zenodo.org/records/1193957, CC BY 4.0.

`vocalset_extract.py` cuts sustained sung notes out of the male singers of VocalSet and writes them as sampler instruments.
`test_vocalset_extract.py` is its test.

## Manifest format

One line per sample: `root_midi layer rr path`.
`root_midi` is the equal-tempered MIDI note the sample was retuned to (A4 = 440 Hz, MIDI 69).
`layer` 0 is the softest layer: pp long tones plus slow_piano scale notes.
`layer` 1 is forte and straight long tones plus slow_forte scale notes.
`rr` counts up from 1 within each (root, layer).
A scale note is only listed for a (root, layer) that has no long-tone sample.
`path` is `/SAMPLES/soundtrack/samples/vocalset_choir/<singer>/<file>.wav`, and the sampler replaces everything up to and including `/soundtrack/samples/` with `$SAMPLES/`.
So `$SAMPLES/vocalset_choir/` must hold the extracted set (the repo keeps it in `/Users/alex/Dev/soundtrack-samples/vocalset_choir/`).
Per singer there are `choir_<singer>_ah.txt` (vowel a) and `choir_<singer>_oh.txt` (vowels o and u).
Wav files are 48 kHz 24-bit mono, named `<singer>_<exercise>_<technique>_<vowel>_m<midi>_<n>.wav`.

## Rebuilding the set from the zip

1. Download VocalSet from https://zenodo.org/records/1193957 and unzip it so that `FULL/<singer>/<exercise>/<technique>/<file>.wav` exists.
2. Run (all paths are arguments, the run takes about 20 s):

```
nice -n 10 python3 -I vocalset_extract.py \
    --vocalset /path/to/vocalset/FULL \
    --out /path/to/soundtrack-samples/vocalset_choir \
    --manifests ../../samples/manifests \
    --tsv vocalset_choir.tsv --md vocalset_choir.md
```

3. The run is deterministic, so the same zip gives byte-identical wavs, manifests and reports.
4. The `.tsv` has one row per written sample (source file, start, end, MIDI, cents correction, f0 sd, vibrato rate and depth, duration, layer, manifest, gain, peak flag).
5. The `.md` lists, per singer, the notes available per vowel and layer, and a pitch coverage table.

## Test

`python3 -I test_vocalset_extract.py` synthesises a 9 s file with three vibrato tones at 20 to 30 cents off equal temperament, 0.5 s gaps, and one 0.9 s blip that is shorter than the 1.2 s minimum.
It asserts three segments, the notes 45, 52 and 57, start and end within 60 ms, the vibrato rate near 5.5 Hz, the written wav at 48 kHz with f0 within 5 cents of equal temperament and RMS -20 dBFS, and that a spiky segment is gain-reduced to a -1 dBFS peak and flagged.
The extractor under test can be swapped with the environment variable `VOCALSET_EXTRACT_PATH`, which is how the mutations below were run.

## Mutation checks

Each mutation was applied to a copy of the extractor, the test was run against it, and the observed failure is listed.
The original was untouched.

| Mutation | Observed failure |
|---|---|
| Skip the pitch correction (resample ratio without the f0 term) | written wav f0 is 25.0 cents from equal temperament (midi 45) |
| Halve the minimum long-tone length (1.2 s to 0.6 s) | expected 3 segments, got 4 |
| Shift the reported MIDI note by +1 | midi: expected 45 got 46 |
| Shift the segment start by +0.1 s | start: expected 0.300 got 0.398 |
| Shift the segment end by +0.1 s | end: expected 2.300 got 2.398 |
| Disable vibrato detection | vibrato rate got 0.00 |
| Do not apply the gain | rms -11.83 dBFS |
| Target 6 dB lower RMS | rms -26.04 dBFS |
| Disable the peak limiter | spiky segment was not flagged as peak limited |
| Round the nearest note to MIDI +1 inside classify | expected 3 segments, got 0 (the 45 cent offset filter rejects every segment first, so the count assertion fires instead of the note one; the row-level MIDI mutation above covers the note assertion) |

## Known limits

The pitch correction lands the median of the tracked frame f0 on the equal-tempered pitch.
When f0 drifts inside a segment (f0 sd 25 to 40 cents is common in the real recordings), a spectral-peak measure of the written wav can differ from the median by more than 5 cents, because the two summarise a moving pitch differently.

# Lyria single-note clips

`lyria_notes_extract.py` measures an AI-generated (Lyria) clip that was asked for eight single held notes, and cuts the clean notes into a sampler instrument.
`test_lyria_notes_extract.py` is its test.
The rules, measurements and thresholds are in the docstring at the top of the extractor.

```
nice python3 -I lyria_notes_extract.py CLIP --name INSTRUMENT [--measure-only] \
    --out-samples /path/to/soundtrack-samples/lyria/INSTRUMENT --manifest lyria_INSTRUMENT.txt \
    --log-rows lyria_rows.tsv --report INSTRUMENT.md [--asked "E4 F#4 ..."]
```

The clip is decoded with ffmpeg to a temporary wav and keeps its sample rate and both channels.
Samples are 24-bit stereo wavs named `<name>_<Note>_<midi>.wav`.
Manifest lines are `root_midi 0 rr /SAMPLES/soundtrack/samples/lyria/<name>/<file>`, so `SAMPLES=/Users/alex/Dev/soundtrack-samples` re-roots them.
The note segmenter is by level only, so it needs real silences between the notes.
Measured on the seven first clips (see the reports in `analysis/lyria_samples/`), the generator did not leave silences: haegeum, trumpet, rhodes and balalaika give no segment, guitar and domra give fragments or one merged segment, and only the cello gives segments, one of which joins three notes.
A fifth chord reads as a single note an octave below with its odd partials missing, so the "one pitch" rule also counts a missing or weak (more than 25 dB down) fundamental as a second pitch.

## Lyria test

`python3 -I test_lyria_notes_extract.py` synthesises a 24.5 s stereo clip: five clean notes (one 30 cents sharp, one with 6 Hz vibrato of 20 cents), a fifth chord, a note with a second attack, a note with an inharmonic second instrument 20 dB down, a 400 cent glide, a 0.4 s blip and a note with band noise at purity 0.93.
It asserts the segment count and boundaries within 20 ms, the measured notes, the sharp offset within 3 cents and the vibrato, the exact rejection reason of each bad segment, the re-measured f0 of the cut sample within 3 cents of equal temperament by an independent harmonic-comb estimator, the cut window (8 ms pre-roll, nothing from the next note), the manifest roots and paths, and round-robin numbering.
The extractor under test can be swapped with `LYRIA_EXTRACT_PATH`, which is how the mutations below were run.

## Lyria mutation checks

Each mutation was applied to a copy of the extractor (the original untouched), the test was run against the copy, and the first assertion that failed is listed.

| Mutation | Observed failure |
|---|---|
| Skip the pitch correction (ratio = 1) | sharp note re-measured: 30.0 cents from equal temperament |
| Purity limit 0 | noisy note at purity limit 0.95: reasons [], expected only purity |
| Ignore second onsets | reattack: expected reasons ['second_onset'] got [] |
| Cut 200 ms late | cut start: expected 8 ms before 2.700, got 2.9000 s |
| Distinct pitches always 1 | chord: expected reasons ['pitch'] got [] |
| Missing-fundamental rule off | chord: expected reasons ['pitch'] got [] |
| Length rule off | blip: expected reasons ['length'] in [] |
| Stability rule off | glide: expected reasons ['stability'] in [] |
| Cut ignores the next note and the release floor | cut end 5.400 s runs into the next note at 4.900 s |
| Manifest root off by one | manifest roots: expected [52, 57, 60, 62, 64, 67] got [53, 58, 61, 63, 65, 68] |
| Round robin always 0 | round robin: [['57', '0', '0'], ['57', '0', '0']] |
| Segment start +0.1 s | start clean: expected 0.500 got 0.600 |
| Segment end +0.1 s | end clean: expected 1.700 got 1.800 |
| Vibrato detection off | vibrato rate: expected 6 got 0.00 |
| Reported MIDI +1 | midi clean: expected 57 got 58 |

## Lyria runs and onsets segmentation

`lyria_notes_extract.py --segment runs|onsets|level` (default `level`, the section above) takes one or more clips, `--layers 0,1,...` (source layer per clip), `--labels` (source text for the log rows, for stems) and `--offsets` (seconds a trimmed stem file starts into the original clip, added to the logged window).
`runs` (legato lines): a candidate is a stretch of voiced YIN frames within 40 cents of the running median, trimmed at both ends until 60 ms are within 25 cents of the median, plus 20 ms of margin, at least 0.45 s.
A candidate is rejected for: not one pitch (the distinct-pitch count), purity under 0.90, a second flux spike inside, a level spread over 9 dB between its quarters, f0 sd over the whole trimmed run over 35 cents, or length.
With the 40 cent limit and the 25 cent trim the sd rule can almost never fire on real material (it is tested on the rule function), so the other rules do the work.
The attack is `natural` when 150 ms before the level onset are 40 dB under the clip peak (cut from 8 ms before the onset, 3 ms fade in), else `none (legato)` (cut from the trimmed start, 12 ms fade in); the fade out is 40 ms.
`onsets` (plucks): a sample runs from 8 ms before a spectral-flux onset to 20 ms before the next onset, the noise floor, or 2.5 s; rejected if under 0.3 s, not one pitch, purity (40 to 300 ms after the onset) under 0.90, or the level 250 ms after the onset above the level 60 ms after it.
Per layer and root MIDI note at most two samples are kept (natural attack first, then purity, then length).
Two layers are only kept if each layer has no gap over 5 semitones between neighbouring roots and spans an octave; otherwise the instrument is one layer from the layer 1 sources and the report says so.
`lyria_clip_survey.py` writes the one-row-per-clip table from the Demucs stems, `lyria_rows_to_md.py` writes `LYRIA_SAMPLES.md` from the log rows.

### Mutations of the new paths

| Mutation | Observed failure |
|---|---|
| Runs: no trimming of the glide | run 0: expected reasons [] got ['second_onset'] |
| Runs: deviation limit 40 to 200 cents | runs: expected 9 candidates got 8 (a semitone step merged) |
| Runs: level-spread rule off | run 3: expected reasons ['level'] got [] |
| Runs: every attack legato | run 0: expected attack natural |
| Runs: every attack natural | run 1: expected attack legato |
| Runs: legato fade in 3 ms | fade lengths |
| Runs: whole-run sd rule off | run_reasons: sd 36 must give stability |
| Runs: second spike rule off | run 5: expected ['second_onset'] got [] |
| Runs: minimum length 0.2 s | run 8: expected ['length'] got [] |
| Select: no natural preference | select_best: expected [2, 3] got [3, 1] |
| Select: three per root | select_best: expected [2, 3] got [2, 3, 1] |
| Layer gap rule off | layer_problem gap |
| Layer range rule off | layer fallback not applied |
| Logged window ignores the offset | logged window start 0.992, expected plus the 10 s offset |
| Log source label ignored | log source label |
| Onsets: entered rule off | pluck 3: expected ['entered'] |
| Onsets: cut ignores the next onset | pluck cut end 3.400, expected the next onset minus 20 ms |
| Onsets: no step back from the flux time | onset chord: expected 3.50 got 3.512 |
| Onsets: minimum length rule off | pluck 4: expected ['length'] got [] |
