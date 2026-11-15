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
