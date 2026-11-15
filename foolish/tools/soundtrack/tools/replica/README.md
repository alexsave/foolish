# replica - how close is a replica render to the original window

compare.py measures a replica against an original window with numbers.
No metric is lenient by construction: onset times are never quantised, matching is one to one, similarities are reported against a chance baseline, and a replica that lacks something scores zero for it.
Run everything with `python3 -I`.

## Usage

```
python3 -I compare.py --orig ORIG.mp3 --replica REPLICA.wav \
    [--orig-start S] [--orig-end S] [--replica-start S] [--replica-end S] \
    [--orig-stems DIR] [--replica-stems DIR] [--no-align] [--out PREFIX]
```

Each stems DIR holds drums.wav, bass.wav, other.wav and optionally vocals.wav.
The start and end windows apply to the stems too.
`--out PREFIX` writes PREFIX.md (human report), PREFIX.tsv (metric, value, unit, scope) and PREFIX.png (plots).
From Python: `compare.compare(orig, replica, sr, stems_orig=None, stems_replica=None, align=True)` returns a flat dict keyed `scope.metric` (scopes mix, drums, bass, other, vocals; global keys have no scope), plus `_units` and `_extra`.
Input is mono-summed, decoded by ffmpeg at 44100 Hz (native rate, used for third-octave spectra, the drum bands and the sidechain), and resampled to 22050 Hz for everything else.
Both sides are scaled to the same overall RMS (0.1); a side's stems get the same gain as its mix.
Different lengths are compared over the shorter.
A stem silent in the original is reported n/a and left out of the mean.
Tests: `python3 -I test_compare.py` (optionally a test letter, for example `... a h`).

## Metric definitions

0. Alignment.
   Onset-strength envelopes (hop 32 samples, 1.45 ms, high-passed) of both mixes are cross-correlated over plus or minus 500 ms with parabolic peak refinement.
   `offset_ms` is positive when the replica is late.
   Periodic music makes a single peak ambiguous by whole grid steps, so the offset is also tracked over consecutive 2 s windows (first window prefers the smallest offset, later ones the peak nearest the extrapolated previous one); the constant offset is the global peak nearest the median tracked offset.
   `drift_ms` is the mean tracked offset of the first third minus that of the last third (a faster replica gives a positive drift).
   The replica (and its stems) is shifted by the offset before every other metric unless `--no-align`.
1. Onsets (mix, drums, bass, other stems).
   Spectral flux of log-compressed magnitudes (1024 window, hop 64 samples = 2.9 ms, max-filtered previous frame), peaks above local mean plus one local standard deviation (1 s window) and above 5 percent of the 99th percentile, at least 20 ms apart, parabolic sub-frame refinement.
   One-to-one matching: all pairs within the tolerance, nearest first, each onset used at most once (greedy, not a maximum matching).
   Reported: precision, recall and F at 30 ms (headline), F at 15 ms and 50 ms, counts, and for the pairs matched at 50 ms the mean signed deviation (replica minus original), the standard deviation and the median absolute deviation (median of |d - median d|), all in ms.
   F with both sides empty is n/a; with one side empty it is 0.
2. Tempo and micro-timing (mix, drums).
   Tempo from the autocorrelation of the onset envelope (50 to 220 BPM, log-Gaussian prior around 110 BPM with one octave sigma), refined by a regression over the first 7 multiples of the beat lag.
   The half and double candidates and the autocorrelation at those lags are reported; a half or double disagreement between the sides is an octave error, not a real tempo difference.
   `tempo_pct_diff` = (replica / orig - 1) * 100.
   Micro-timing: the original tempo and a 16th-grid phase fitted to the original (the phase maximising a Gaussian, sigma 6 ms, count of the original's onsets near grid lines; beat start = the slot with the most onset strength).
   Each side's onsets are folded onto 4 slots per beat; per slot the mean deviation from the exact 16th in ms and the share of onsets.
   `swing_ms` = mean deviation of slots 1 and 3 minus mean of slots 0 and 2.
   `microtiming_profile_mad_ms` = mean over slots of |orig mean dev - replica mean dev|.
   Diagnostic only; nothing is moved to the grid for metric 1.
3. Harmony (mix, other, bass).
   Log-frequency spectrogram from an 8192 STFT at 22050 Hz, hop 1024 (46 ms), 36 bins per octave from 55 Hz to 7 kHz (linear interpolation of the STFT magnitude).
   Chroma: bins summed to 12 pitch classes by nearest semitone.
   `chroma_cos` is the mean over frames of the cosine of the chroma vectors; `cqt_cos` the same on log1p(1000 * magnitude) of the full frame.
   Frames where both sides are below -60 dB of their own peak are skipped.
   The chance baseline is the same number after circularly rolling the replica by 7, 13 and 19 s (for clips shorter than 40 s, by 7/42, 13/42 and 19/42 of the duration), averaged.
   `*_above_chance` = (value - chance) / (1 - chance).
4. Pitch (bass: 30 to 400 Hz; other: 80 to 800 Hz, polyphonic so untrustworthy).
   YIN (1024 window, hop 512, threshold 0.15), voiced when periodicity (1 - normalised difference) is above 0.75 and the frame is within 45 dB of the stem's loudest.
   `pitch_voiced_overlap` = share of original-voiced frames also voiced in the replica; `pitch_within50` = share of frames voiced in both within 50 cents; `pitch_within50_octave` allows octave errors.
5. Drum pattern (drums stem if given, else the mix; scope drums or mix).
   Zero-phase band-pass energy in dB per stream: kick 30-120 Hz, clap 1.5-5 kHz, hat 7-11 kHz (native rate).
   Onsets are rises of at least 6 dB (hat 4 dB) over 3 frames, picked adaptively; the dB floor is 45 dB below the louder side's peak in the band (shared by both sides, so a replica that lacks a band does not get its noise amplified into onsets).
   Counts, precision, recall and F at 30 ms; for matched hits `strength_corr` (Pearson of the peak band energy in dB in the 50 ms after the onset) and `strength_absdiff_db` (mean absolute difference after removing the median difference).
6. Spectrum and brightness (mix and every stem).
   Welch spectrum in 28 third-octave bands, 31.5 Hz to 16 kHz, native rate, both normalised to equal total level; per-band dB difference (replica minus original) is in the report; `lts_mad_db` = mean absolute difference over bands whose original level is within 50 dB of the loudest band.
   Brightness: spectral centroid per 100 ms frame; `bright_corr` Pearson, `bright_median_ratio` median of replica / original, over frames where either side is within 50 dB of its peak.
7. Loudness (mix and every stem).
   RMS in dB over 400 ms windows hopped 100 ms, both sides at equal overall RMS, levels floored at 60 dB below the side's peak; `loud400_corr` Pearson and `loud400_mad_db` mean absolute difference over frames where either side is within 50 dB of its peak (a replica silent where the original is loud counts against it).
   `loud20_corr`: the same correlation with 20 ms windows hopped 10 ms (pumping).
8. Sidechain pump (needs stems).
   Kicks are the kick-stream onsets of each side's drums stem; level of that side's other + bass stems.
   Per kick: level in 20-80 ms after the kick minus the level in 150-250 ms after it, in dB; `pump_db_orig` and `pump_db_replica` are the medians (negative means ducked).
   `pump_recovery_ms_*`: median time from the kick until the 10 ms level is within 1 dB of the 150-250 ms level and stays so for 50 ms (0 when there is no duck).
9. Overall.
   100 times the mean of: onset F@30 mix, drums-stem onset F@30 (mix when there are no stems), kick F@30, clap F@30, hat F@30, clip01(chroma_cos_above_chance), clip01(cqt_cos_above_chance), bass `pitch_within50` times `pitch_voiced_overlap` (omitted without a bass stem), max(0, 1 - lts_mad_db / 12), max(0, loud400_corr), max(0, bright_corr).
   n/a terms are left out of the mean.
   An identical pair scores 100.

The report also lists the 10 worst 2 s windows by chroma similarity and by onset F@30 (mix), so a human can see where the replica is off.
The PNG shows the onset envelopes (original up, replica mirrored down; matched, missed and extra onsets marked), the chroma cosine series, the loudness curves and the third-octave spectra.

## Mutation table

Each row: one deliberate break of compare.py, the test that went red, and the failing assertion.
compare.py was restored from a copy after each and a final diff against the first backup was empty.

| test | mutation | assertion that failed |
|---|---|---|
| a identical | every replica onset shifted +40 ms before matching | identical: mix.onset_f15 = 0.0, expected 1 |
| b delay 120 ms | alignment returns offset 0 | delay: offset_ms = 0.0, expected 120 +-3 |
| c quantised | onset tolerance widened 10x (300 ms at the 30 ms setting) | quantised: mix onset_f30 = 0.99, expected < 0.6 |
| d transposed | chroma ignores pitch class (all bins into one class) | transposed: chroma_cos_above_chance = nan, expected < 0.5 |
| e no hats | hat recall forced to 1 | no hats: hat_recall = 1.0, expected < 0.1 |
| f half kicks | hit strengths all zero | half kicks: kick_strength_absdiff_db = 0.0, expected > 2 |
| g lowpass | lts_mad_db forced to 0 | lowpass: lts_mad_db = 0.0, expected > 3 |
| h 3 percent faster | drift_ms forced to 0 | faster: drift_ms = 0.0, expected abs > 100 |
| i sidechain | both pump windows set to the late window | sidechain: pump_db_orig = 0.0, expected -5.24 +-2 |
| j unrelated song | chance baseline forced to 0 | unrelated: chroma_cos_above_chance = 0.578, expected < 0.3 |
| k gain (extra) | loudness normalisation skipped | gain: overall = 98.68, expected >= 99.5 |
| l robustness (extra) | silent-stem detection disabled | robust: silent vocals stem not reported |

## Known limits

The synthetic tests tune thresholds to synthetic signals, not to real music.
Onset detection on dense pads and noisy mixes is noisy; the same detector runs on both sides, so its bias largely cancels, but its misses do not.
The constant offset cannot follow a tempo mismatch; read `drift_ms`.
Chroma and CQT cosines of non-negative vectors are high even for unrelated music (about 0.6 to 0.8), which is why only the above-chance values enter the overall score.
Tempo can land on half or double; compare the candidates before trusting `tempo_pct_diff`.

## extract.py, assemble.py, play.c and test_extract.sh: replaying a window from its own stems

These four make a replica of a window of a recording that plays from an editable score with the recording's real timing, using a small set of one-shot samples cut from the recording's own separated stems.
Run Python with `python3 -I`.
The measured trade-off between how few samples are stored and how close the replica gets lives with each replica (its work folder, TRADEOFF.md).

### Usage

```
python3 -I extract.py --stem STEM.wav [--add MORE.wav ...] --name PART --start S --end S --bpm B --distance D \
    --samples-dir DIR --manifest PATH --events PATH --table PATH [--lead-ms 6] [--min-seg-ms 50] \
    [--timing trim|attack|segment] [--feat-eps 0.01] [--split-roots]
python3 -I assemble.py --dir REPLICA_DIR --bpm B --bars N --part NAME EVENTS MANIFEST GROUP [--part ...] [--manifest-names]
clang -O2 -w -o play ../../replica/play.c        # from this directory
cd REPLICA_DIR && SAMPLES=<samples root> ENGINE=<engine dir> ./play full OUTDIR        # SOLO=drums MGAIN=<dB> for one part
bash test_extract.sh
```

extract.py cuts the stem window, detects onsets with compare.py's detector (never quantised), cuts one segment per onset (from onset minus the lead to the next onset minus the lead), clusters the segments by spectral shape (complete linkage on 1 minus cosine at distance D, D = 0 keeps every segment), stores the longest member of each cluster as a float32 wav at the stem's native rate (raw level, 2 ms fade in, 8 ms fade out), and writes one event per segment.
The manifest line is `root 0 0 path` with root = 24 + cluster index (0 + index above 100 clusters).
A manifest path under `.../soundtrack-samples/` is written as `/soundtrack/samples/...`, which sampler.h re-roots onto `$SAMPLES`.
Events are `EVENTS PART: beat,pitch,len,vel` lines of at most 40 events; the first line of the events file is the comment `# PART gain_db X`.
The velocity is v = clip(r / r_max - 0.05, 0, 0.95) with r the member's RMS over its first 100 ms divided by the exemplar's, and the part gain is 20 log10(r_max), so the sampler's gain (v + 0.05) times the part gain reproduces r.
The table (tsv) has one row per segment: start s, length s, cluster, exemplar, r, v, distance to its exemplar, then the cut as made (start and end in the clip's time).

assemble.py writes the replica directory's score.txt (TEMPO, one SECTION, the events) and parts.txt.
play.c (in `foolish/tools/soundtrack/replica/`) is the one player for every replica directory: sampler tracks with `sp[15] = 1` (exact: no random detune, no gain variation), velocity exponent 1, no loop, no pan, no sends, no EQ, no compression, no humanise, no swing, and a master with nothing but the engine's -16 LUFS normalisation and limiter.
parts.txt is `PART <name> sampler <manifest> gain_db <x> release_s <x> group <drums|bass|other> [exact 0|1]`; an unknown keyword or an unknown part in the score stops the run.
`sampler` may be repeated: the k-th manifest answers events of pitch 128k + root (extract.py `--split-roots`, needed above 128 clusters because a MIDI root only goes to 127).

### Choices the brief left open, and what was measured

- The feature's compression is `log1p(magnitude / (segment peak * eps))` with eps 0.01 (`--feat-eps`).
  The first try, `log1p(1e4 * magnitude)` in absolute level, is so compressed that D = 0.2 merges the whole bass stem into 1-2 clusters.
  On the synthetic test (an earlier, single-chord version of the song) at D = 0.05, eps 0.3 / 0.1 / 0.03 / 0.01 / 0.003 gave overall 87.1 / 87.1 / 88.9 / 91.9 / 85.2, so 0.01.
- Timing.
  The engine's `smp_trim` drops everything before the first sample that reaches 0.3 percent of the peak (-50 dB), less 1.5 ms.
  A sample that starts with the 2 ms fade-in and sound loses nothing (the first sample over -50 dB is within 0.1 ms, and 1.5 ms are kept), but a sample with S ms of pre-roll below -50 dB loses S - 1.5 ms, so its sound lands that much early.
  On the synthetic song (digital silence between sounds) `--timing segment`, the literal segment start, lands the replica 21.5 ms early with a 10.9 ms mean signed onset deviation and the test fails; `--timing trim` (the default: the event starts later by what smp_trim drops from the exemplar) lands it within 0.3 ms.
  On the real stems smp_trim dropped nothing (there is always sound above -50 dB in the 6 ms before an onset), so `trim` and `segment` gave identical numbers there.
  `--timing attack` finds each segment's attack by one rule (first 1 ms block at 20 percent of the loudest block of the first 80 ms) and lands attacks instead of segment starts; it is as good on the synthetic song, and on real stems it moves offsets by a few ms in either direction (bass D = 0.1: offset -6.3 ms against 0.04 ms), because compare.py's own detector judges the replica's onsets by its own rule.
  The detector itself fires from 17 ms before the attack (noise bursts) to 5 ms after it (a kick with a click) depending on the sound, which is why the lead is 6 ms.
- The engine runs at 48 kHz and the stems are 44.1 kHz, so the sampler resamples every sample (cubic, ratio 0.91875).
  The measured level error of that is below 0.001 dB on a 440 Hz tone (test: -6.021 dB for velocity 0.45, which is 20 log10(0.5) = -6.0206).

### test_extract.sh

A deterministic 12 s synthetic song (test_compare.py's builder, its one seed; swung off 16ths 40 and 34 ms late with plus or minus 4 ms of jitter, one chord, the kick with a 3 ms click, the pad gated into stabs and the bass into plucks so they have onsets and a loudness shape) is split into three stems, replayed at D = 0.05 through extract.py, play.c and compare.py, and asserted: overall >= 90, mix onset F@30 >= 0.95, |mean signed onset deviation| <= 3 ms, drums onset F@30 >= 0.95, each stem's 400 ms loudness correlation >= 0.9, at most 8 drum samples, and (extra) a constant offset within 4 ms and the lead honoured (the first burst's attack sits 6 ms later with `--lead-ms 12` than with 6).
A second check renders 10 isolated notes of velocity 0.45 (v + 0.05 = 0.5) with exact on and off: with it each is -6.021 dB against the stored sample (limit +-0.05 around -6.02), without it they scatter by up to 0.9 dB.
The test also takes the environment variables EXTRACT, PLAYC and TMP, which is how the mutations below were run on copies.
The stock song's pad and bass are stationary, so a 400 ms loudness curve of them is flat to 0.5 dB and no replica correlates with it (measured 0.50 for the pad); that is why the test gates them.

### Mutation table (extract.py and sampler.h copies, restored afterwards; a diff against the originals was empty)

| mutation | assertions that failed |
|---|---|
| events rounded to a 16th at the tempo | mix_onset_f30 = 0.795 (expected >= 0.95), onset_mean_dev = -7.77 ms (expected within 3), drums_onset_f30 = 0.802, loud400_bass = 0.635, overall = 82.4 |
| all velocities 0.95 | loud400_drums = 0.724 (expected >= 0.9), overall = 85.6 |
| exemplar chosen as the shortest member | drums_onset_f30 = 0.932 (expected >= 0.95), loud400_bass = 0.691, overall = 82.4 |
| cluster-to-root mapping shifted by one | mix_onset_f30 = 0.848, onset_mean_dev = 12.66 ms, constant_offset = 232 ms, drums_onset_f30 = 0.847, loud400 of all three stems (0.16, -0.32, 0.21), overall = 46.7 |
| the exact flag ignored (sampler.h) | exact_level: worst deviation from -6.02 dB = 0.921 dB (expected <= 0.05); loud400_bass = 0.853 |
| the lead subtracted twice | lead: attack position with --lead-ms 12 minus with 6 = 12.97 ms (expected 6.00 +-0.3) |

### Known limits

Onset-driven segmentation cuts a stationary sustained sound (a pad) at spurious onsets into fragments that replay with the wrong envelope.
Members of a cluster are all played as the exemplar at the exemplar's pitch: there is no transposition, so a cluster that merges two pitches plays one of them wrong.
The velocity is read from the first 100 ms only, and a cluster whose exemplar starts quietly makes r_max (and so the part gain) large.


## extract.py, iteration 2: pitch-aware clustering

`--roots spaced` stores sample i at root 100 * i + 50 and plays an event at pitch root + shift (semitones, fractional allowed, 4 decimals).
The events file's first line then ends in `roots spaced`, assemble.py copies that as the parts.txt keyword `roots spaced`, and play.c skips its pitch >= 127.5 manifest routing for that part.
A parts.txt without the keyword renders exactly as before (the committed se11_drop_0-42_mid renders sample-identical).
`--transpose f0` is for a monophonic stem (bass).
Per segment it measures the median f0 with compare.py's yin (`--f0-range 30 400`), the voiced share and f0 at 25, 50 and 75 percent.
Features are compared after shifting each by its f0 (36 bins per octave), an event's shift is 12 log2(f0 member / f0 exemplar), limited to `--max-shift` (default 12) by splitting off a member beyond it.
Segments with a voiced share under 0.5 cluster only among themselves at shift 0, and the exemplar is the longest member with a voiced share of at least 0.8.
`--transpose shift` is for a polyphonic stem (other).
The distance is the minimum over integer k in plus or minus `--max-shift` (default 5) semitones of 1 - cosine(A, B moved up k semitones), with k = 0 winning ties within 1e-6, and a member's shift is the k that matches the exemplar to it.
`--merge-sustain TAU` removes an onset when the 80 ms of feature before it and after it are closer than TAU and the 20 ms RMS does not rise by more than 3 dB; the number removed is printed.
Velocity is now the RMS ratio over the common length of member and exemplar (the exemplar as transposed); `--vel-window-ms 100` is the old rule.
The table gains the columns f0, shift, voiced, f0_25, f0_50, f0_75 and short_ms (how much longer the member is than the transposed exemplar), and extract.py prints a STATS line (boundaries merged, median absolute shift, events short by more than 30 ms, splits, median glide in cents, unvoiced segments).
Tests: test_extract.sh covers a bass line, chord stabs at four transpositions and a tremolo pad.

### Iteration 2 tradeoff on se_11, 0 to 42 s, 160 BPM (each stem rendered and compared alone)

Drums rows are iteration 1 (D given, method unchanged).
`maxshift` is the shift limit in semitones; rising_* count samples whose 10 ms RMS envelope peaks after 50 percent of their length with the first 30 ms more than 9 dB below the peak.

| stem | options | D | merge | maxshift | events | samples | stored_share | part_gain_db | merged | median_abs_shift | short30 | offset_ms | onset_f30 | onset_dev_mean_ms | chroma_above_chance | cqt_above_chance | lts_mad_db | loud400_corr | loud400_mad_db | loud20_corr | bright_corr | bass_pitch_within50 | rising_samples | rising_events | rising_play_s |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| drums | D=0 (iteration 1, reused) | 0 | - | - | 192 | 192 | 1 | 0.9834 | - | 0 | - | -0.3034 | 1 | -0.3284 | 0.9916 | 0.9858 | 0.2891 | 1 | 0.06232 | 0.9991 | 0.9988 | - | 0 | 0 | 0 |
| drums | D=0.02 (iteration 1, reused) | 0.02 | - | - | 192 | 86 | 0.463 | 4.437 | - | 0 | - | 0.7896 | 0.9641 | -0.5467 | 0.8718 | 0.9058 | 0.6822 | 0.9965 | 0.3569 | 0.9588 | 0.8786 | - | 0 | 0 | 0 |
| drums | D=0.05 (iteration 1, reused) | 0.05 | - | - | 192 | 35 | 0.2126 | 12.87 | - | 0 | - | 1.553 | 0.961 | -1.017 | 0.7057 | 0.7961 | 1.256 | 0.979 | 0.5418 | 0.9012 | 0.7544 | - | 0 | 0 | 0 |
| drums | D=0.1 (iteration 1, reused) | 0.1 | - | - | 192 | 20 | 0.1366 | 12.87 | - | 0 | - | 1.681 | 0.9509 | -1.232 | 0.6499 | 0.7491 | 1.308 | 0.9775 | 0.591 | 0.8833 | 0.7098 | - | 0 | 0 | 0 |
| drums | D=0.2 (iteration 1, reused) | 0.2 | - | - | 192 | 9 | 0.0807 | 12.87 | - | 0 | - | 1.822 | 0.9372 | -2.769 | 0.6406 | 0.7296 | 1.713 | 0.9783 | 0.5833 | 0.869 | 0.6567 | - | 0 | 0 | 0 |
| drums | D=0.35 (iteration 1, reused) | 0.35 | - | - | 192 | 5 | 0.0526 | 15.32 | - | 0 | - | 1.991 | 0.9364 | -3.366 | 0.602 | 0.6863 | 1.904 | 0.9781 | 0.5951 | 0.8571 | 0.6601 | - | 0 | 0 | 0 |
| bass | --transpose f0 --roots spaced | 0.02 | off | 12 | 224 | 53 | 0.334 | 27.99 | 0 | 0 | 11 | -8.077 | 0.6356 | -2.785 | 0.673 | 0.4997 | 1.184 | 0.9351 | 1.773 | 0.6977 | 0.5545 | 0.8018 | 12 | 52 | 9.234 |
| bass | --transpose f0 --roots spaced --max-shift 2 | 0.02 | off | 2 | 224 | 53 | 0.334 | 27.99 | 0 | 0 | 11 | -8.077 | 0.6356 | -2.785 | 0.673 | 0.4997 | 1.184 | 0.9351 | 1.773 | 0.6977 | 0.5545 | 0.8018 | 12 | 52 | 9.234 |
| bass | --transpose f0 --roots spaced --merge-sustain 0.05 | 0.02 | 0.05 | 12 | 55 | 23 | 0.6 | 6.771 | 169 | 0 | 1 | 0.02852 | 0.7333 | -1.709 | 0.7623 | 0.7497 | 0.343 | 0.9259 | 1.33 | 0.7798 | 0.6896 | 0.8341 | 6 | 15 | 10.06 |
| bass | --transpose f0 --roots spaced --merge-sustain 0.05 --max-shift 2 | 0.02 | 0.05 | 2 | 55 | 23 | 0.6 | 6.771 | 169 | 0 | 1 | 0.02852 | 0.7333 | -1.709 | 0.7623 | 0.7497 | 0.343 | 0.9259 | 1.33 | 0.7798 | 0.6896 | 0.8341 | 6 | 15 | 10.06 |
| bass | --transpose f0 --roots spaced | 0.05 | off | 12 | 224 | 20 | 0.1803 | 27.99 | 0 | 0.0153 | 5 | -9.433 | 0.5677 | -2.722 | 0.6374 | 0.4496 | 1.523 | 0.921 | 2.119 | 0.676 | 0.6748 | 0.718 | 6 | 94 | 15.85 |
| bass | --transpose f0 --roots spaced --max-shift 2 | 0.05 | off | 2 | 224 | 20 | 0.1803 | 27.99 | 0 | 0.0153 | 5 | -9.433 | 0.5677 | -2.722 | 0.6374 | 0.4496 | 1.523 | 0.921 | 2.119 | 0.676 | 0.6748 | 0.718 | 6 | 94 | 15.85 |
| bass | --transpose f0 --roots spaced --merge-sustain 0.05 | 0.05 | 0.05 | 12 | 55 | 12 | 0.3286 | 9.038 | 169 | 0.0159 | 2 | -2.436 | 0.5782 | 0.7728 | 0.6038 | 0.55 | 0.7636 | 0.7399 | 3.066 | 0.5852 | 0.213 | 0.7125 | 3 | 6 | 2.696 |
| bass | --transpose f0 --roots spaced --merge-sustain 0.05 --max-shift 2 | 0.05 | 0.05 | 2 | 55 | 12 | 0.3286 | 9.038 | 169 | 0.0159 | 2 | -2.436 | 0.5782 | 0.7728 | 0.6038 | 0.55 | 0.7636 | 0.7399 | 3.066 | 0.5852 | 0.213 | 0.7125 | 3 | 6 | 2.696 |
| bass | --transpose f0 --roots spaced | 0.1 | off | 12 | 224 | 12 | 0.1439 | 27.99 | 0 | 0.0286 | 4 | -9.8 | 0.5648 | -2.384 | 0.6177 | 0.426 | 1.96 | 0.9137 | 2.186 | 0.6951 | 0.7395 | 0.7005 | 3 | 124 | 19.53 |
| bass | --transpose f0 --roots spaced --max-shift 2 | 0.1 | off | 2 | 224 | 13 | 0.1483 | 27.99 | 0 | 0.0278 | 4 | -9.696 | 0.5648 | -2.462 | 0.6305 | 0.4321 | 1.959 | 0.9137 | 2.183 | 0.6956 | 0.7393 | 0.7109 | 3 | 122 | 19.27 |
| bass | --transpose f0 --roots spaced --merge-sustain 0.05 | 0.1 | 0.05 | 12 | 55 | 8 | 0.2518 | 9.038 | 169 | 0.0338 | 4 | -4.85 | 0.5213 | 1.764 | 0.5413 | 0.4847 | 0.9233 | 0.6565 | 3.747 | 0.4732 | 0.144 | 0.6582 | 1 | 4 | 2.325 |
| bass | --transpose f0 --roots spaced --merge-sustain 0.05 --max-shift 2 | 0.1 | 0.05 | 2 | 55 | 10 | 0.2765 | 9.038 | 169 | 0.0214 | 4 | -3.628 | 0.5309 | 0.9106 | 0.5551 | 0.4949 | 0.897 | 0.6585 | 3.703 | 0.477 | 0.1439 | 0.679 | 3 | 8 | 4.412 |
| bass | --transpose f0 --roots spaced | 0.2 | off | 12 | 224 | 6 | 0.1074 | 28.87 | 0 | 1.49 | 0 | -3.628 | 0.6908 | -5.222 | 0.4047 | 0.3059 | 1.457 | 0.9077 | 2.502 | 0.7337 | 0.8103 | 0.2664 | 1 | 77 | 11.91 |
| bass | --transpose f0 --roots spaced --max-shift 2 | 0.2 | off | 2 | 224 | 10 | 0.1225 | 28.87 | 0 | 0.0785 | 4 | -2.904 | 0.5626 | 0.1165 | 0.5202 | 0.3886 | 1.738 | 0.9204 | 2.238 | 0.728 | 0.7264 | 0.4945 | 2 | 74 | 11.57 |
| bass | --transpose f0 --roots spaced --merge-sustain 0.05 | 0.2 | 0.05 | 12 | 55 | 5 | 0.2212 | 9.038 | 169 | 0.183 | 1 | -3.292 | 0.5166 | 0.7535 | 0.47 | 0.4384 | 1.231 | 0.7247 | 3.371 | 0.5937 | 0.2335 | 0.4955 | 0 | 0 | 0 |
| bass | --transpose f0 --roots spaced --merge-sustain 0.05 --max-shift 2 | 0.2 | 0.05 | 2 | 55 | 9 | 0.2762 | 9.038 | 169 | 0.0214 | 2 | -3.153 | 0.5391 | 1.591 | 0.554 | 0.4959 | 0.9438 | 0.7316 | 3.12 | 0.579 | 0.211 | 0.641 | 3 | 6 | 2.696 |
| bass | --transpose f0 --roots spaced | 0.35 | off | 12 | 224 | 5 | 0.0943 | 28.87 | 0 | 1.533 | 1 | -5.146 | 0.6867 | -4.028 | 0.3644 | 0.2757 | 3.659 | 0.9027 | 2.581 | 0.6883 | 0.6733 | 0.241 | 1 | 77 | 11.91 |
| bass | --transpose f0 --roots spaced --max-shift 2 | 0.35 | off | 2 | 224 | 10 | 0.1225 | 28.87 | 0 | 0.0785 | 4 | -2.904 | 0.5626 | 0.1165 | 0.5202 | 0.3886 | 1.738 | 0.9204 | 2.238 | 0.728 | 0.7264 | 0.4945 | 2 | 74 | 11.57 |
| bass | --transpose f0 --roots spaced --merge-sustain 0.05 | 0.35 | 0.05 | 12 | 55 | 3 | 0.144 | 9.038 | 169 | 0.6804 | 1 | -2.707 | 0.4713 | 1.108 | 0.351 | 0.3606 | 3.51 | 0.7166 | 3.674 | 0.5612 | 0.1928 | 0.4045 | 0 | 0 | 0 |
| bass | --transpose f0 --roots spaced --merge-sustain 0.05 --max-shift 2 | 0.35 | 0.05 | 2 | 55 | 8 | 0.2367 | 9.038 | 169 | 0.0271 | 3 | -1.756 | 0.5124 | 0.1298 | 0.5056 | 0.4516 | 3.484 | 0.6392 | 3.931 | 0.4987 | 0.1162 | 0.6222 | 3 | 6 | 2.696 |
| other | --transpose shift --roots spaced | 0.02 | off | 5 | 225 | 162 | 0.7253 | 5.566 | 0 | 0 | 0 | -1.464 | 0.8271 | 0.3704 | 0.9401 | 0.8501 | 0.2539 | 0.9845 | 0.2196 | 0.9198 | 0.9131 | - | 3 | 3 | 0.9843 |
| other | --transpose shift --roots spaced --max-shift 2 | 0.02 | off | 2 | 225 | 162 | 0.7253 | 5.566 | 0 | 0 | 0 | -1.464 | 0.8271 | 0.3704 | 0.9401 | 0.8501 | 0.2539 | 0.9845 | 0.2196 | 0.9198 | 0.9131 | - | 3 | 3 | 0.9843 |
| other | --transpose shift --roots spaced --merge-sustain 0.03 | 0.02 | 0.03 | 5 | 154 | 120 | 0.7666 | 2.161 | 71 | 0 | 0 | -1.151 | 0.8265 | 0.04194 | 0.9433 | 0.872 | 0.3953 | 0.9866 | 0.2144 | 0.9299 | 0.8599 | - | 6 | 9 | 4.742 |
| other | --transpose shift --roots spaced --merge-sustain 0.03 --max-shift 2 | 0.02 | 0.03 | 2 | 154 | 120 | 0.7666 | 2.161 | 71 | 0 | 0 | -1.151 | 0.8265 | 0.04194 | 0.9433 | 0.872 | 0.3953 | 0.9866 | 0.2144 | 0.9299 | 0.8599 | - | 6 | 9 | 4.742 |
| other | --transpose shift --roots spaced --merge-sustain 0.06 | 0.02 | 0.06 | 5 | 74 | 60 | 0.8829 | 2.901 | 151 | 0 | 0 | -1.128 | 0.8916 | 1.077 | 0.9452 | 0.9193 | 0.3657 | 0.9752 | 0.2184 | 0.9296 | 0.9309 | - | 5 | 5 | 3.797 |
| other | --transpose shift --roots spaced --merge-sustain 0.06 --max-shift 2 | 0.02 | 0.06 | 2 | 74 | 60 | 0.8829 | 2.901 | 151 | 0 | 0 | -1.128 | 0.8916 | 1.077 | 0.9452 | 0.9193 | 0.3657 | 0.9752 | 0.2184 | 0.9296 | 0.9309 | - | 5 | 5 | 3.797 |
| other | --transpose shift --roots spaced --merge-sustain 0.1 | 0.02 | 0.1 | 5 | 49 | 35 | 0.8576 | 2.219 | 176 | 0 | 0 | -1.119 | 0.86 | 0.7721 | 0.9319 | 0.9075 | 0.3919 | 0.9725 | 0.25 | 0.9162 | 0.9252 | - | 2 | 2 | 2.235 |
| other | --transpose shift --roots spaced --merge-sustain 0.1 --max-shift 2 | 0.02 | 0.1 | 2 | 49 | 35 | 0.8576 | 2.219 | 176 | 0 | 0 | -1.119 | 0.86 | 0.7721 | 0.9319 | 0.9075 | 0.3919 | 0.9725 | 0.25 | 0.9162 | 0.9252 | - | 2 | 2 | 2.235 |
| other | --transpose shift --roots spaced | 0.05 | off | 5 | 225 | 54 | 0.3344 | 6.148 | 0 | 0 | 0 | -1.958 | 0.6564 | -1.857 | 0.7144 | 0.6008 | 0.9689 | 0.966 | 0.4089 | 0.7988 | 0.7156 | - | 1 | 5 | 1.681 |
| other | --transpose shift --roots spaced --max-shift 2 | 0.05 | off | 2 | 225 | 55 | 0.3411 | 5.912 | 0 | 0 | 0 | -2.177 | 0.6679 | -1.529 | 0.7305 | 0.609 | 0.9511 | 0.9701 | 0.3896 | 0.8063 | 0.7217 | - | 1 | 5 | 1.681 |
| other | --transpose shift --roots spaced --merge-sustain 0.03 | 0.05 | 0.03 | 5 | 154 | 47 | 0.3532 | 5.912 | 71 | 0 | 0 | -3.932 | 0.6103 | -0.02927 | 0.6362 | 0.5649 | 1.004 | 0.934 | 0.532 | 0.6836 | 0.6476 | - | 2 | 8 | 3.025 |
| other | --transpose shift --roots spaced --merge-sustain 0.03 --max-shift 2 | 0.05 | 0.03 | 2 | 154 | 47 | 0.3525 | 5.912 | 71 | 0 | 0 | -3.628 | 0.6188 | -0.7029 | 0.6264 | 0.561 | 1.005 | 0.9309 | 0.5632 | 0.6723 | 0.6436 | - | 2 | 8 | 3.025 |
| other | --transpose shift --roots spaced --merge-sustain 0.06 | 0.05 | 0.06 | 5 | 74 | 27 | 0.5024 | 3.139 | 151 | 0 | 0 | -1.58 | 0.6746 | 1.563 | 0.6853 | 0.6545 | 0.9053 | 0.885 | 0.6728 | 0.6858 | 0.6881 | - | 2 | 10 | 9.407 |
| other | --transpose shift --roots spaced --merge-sustain 0.06 --max-shift 2 | 0.05 | 0.06 | 2 | 74 | 27 | 0.5024 | 3.139 | 151 | 0 | 0 | -1.58 | 0.6746 | 1.563 | 0.6853 | 0.6545 | 0.9053 | 0.885 | 0.6728 | 0.6858 | 0.6881 | - | 2 | 10 | 9.407 |
| other | --transpose shift --roots spaced --merge-sustain 0.1 | 0.05 | 0.1 | 5 | 49 | 16 | 0.4216 | 4.133 | 176 | 0 | 2 | -2.749 | 0.6632 | 0.7135 | 0.5668 | 0.5519 | 0.5766 | 0.8402 | 0.797 | 0.4718 | 0.5912 | - | 0 | 0 | 0 |
| other | --transpose shift --roots spaced --merge-sustain 0.1 --max-shift 2 | 0.05 | 0.1 | 2 | 49 | 17 | 0.4407 | 4.133 | 176 | 0 | 1 | -1.796 | 0.6792 | 1.34 | 0.5685 | 0.5265 | 0.5718 | 0.8699 | 0.7555 | 0.6532 | 0.5924 | - | 0 | 0 | 0 |
| other | --transpose shift --roots spaced | 0.1 | off | 5 | 225 | 16 | 0.1319 | 6.093 | 0 | 0 | 0 | -0.692 | 0.6606 | 6.567 | 0.4475 | 0.3786 | 2.13 | 0.9509 | 0.5367 | 0.7218 | 0.6396 | - | 1 | 116 | 23.27 |
| other | --transpose shift --roots spaced --max-shift 2 | 0.1 | off | 2 | 225 | 19 | 0.1571 | 6.093 | 0 | 0 | 0 | -1.316 | 0.7423 | 0.9904 | 0.5346 | 0.4446 | 1.996 | 0.9697 | 0.4271 | 0.7783 | 0.6229 | - | 1 | 25 | 5.042 |
| other | --transpose shift --roots spaced --merge-sustain 0.03 | 0.1 | 0.03 | 5 | 154 | 16 | 0.1591 | 6.093 | 71 | 0 | 0 | -10.42 | 0.5439 | 2.439 | 0.4204 | 0.377 | 1.244 | 0.9231 | 0.6561 | 0.6148 | 0.437 | - | 0 | 0 | 0 |
| other | --transpose shift --roots spaced --merge-sustain 0.03 --max-shift 2 | 0.1 | 0.03 | 2 | 154 | 16 | 0.1664 | 6.093 | 71 | 0 | 0 | -9.433 | 0.5285 | 4.795 | 0.4237 | 0.3894 | 1.213 | 0.915 | 0.6922 | 0.604 | 0.4762 | - | 1 | 29 | 9.909 |
| other | --transpose shift --roots spaced --merge-sustain 0.06 | 0.1 | 0.06 | 5 | 74 | 10 | 0.2404 | 6.079 | 151 | 0 | 0 | -372.2 | 0.4263 | 1.095 | 0.1103 | 0.1627 | 1.472 | 0.5229 | 1.611 | 0.2434 | 0.1326 | - | 1 | 17 | 12.45 |
| other | --transpose shift --roots spaced --merge-sustain 0.06 --max-shift 2 | 0.1 | 0.06 | 2 | 74 | 10 | 0.2418 | 4.149 | 151 | 0 | 0 | -372.2 | 0.4286 | 0.3729 | 0.1302 | 0.1724 | 1.469 | 0.5174 | 1.625 | 0.2392 | 0.1061 | - | 1 | 17 | 12.45 |
| other | --transpose shift --roots spaced --merge-sustain 0.1 | 0.1 | 0.1 | 5 | 49 | 6 | 0.2046 | 1.561 | 176 | 0 | 1 | -2.061 | 0.4967 | 1.295 | 0.3111 | 0.2632 | 1.067 | 0.7794 | 1.015 | 0.3716 | 0.4468 | - | 0 | 0 | 0 |
| other | --transpose shift --roots spaced --merge-sustain 0.1 --max-shift 2 | 0.1 | 0.1 | 2 | 49 | 7 | 0.2544 | 1.561 | 176 | 0 | 0 | -1.581 | 0.5241 | 2.171 | 0.3507 | 0.3044 | 1.063 | 0.8591 | 0.879 | 0.586 | 0.4362 | - | 0 | 0 | 0 |
| other | --transpose shift --roots spaced | 0.2 | off | 5 | 225 | 3 | 0.0402 | 5.974 | 0 | 0 | 0 | -1.974 | 0.8039 | 1.339 | 0.2441 | 0.2422 | 1.838 | 0.925 | 0.5777 | 0.7346 | -0.05157 | - | 0 | 0 | 0 |
| other | --transpose shift --roots spaced --max-shift 2 | 0.2 | off | 2 | 225 | 4 | 0.0613 | 9.614 | 0 | 0 | 0 | 3.628 | 0.7201 | 6.981 | 0.2971 | 0.2508 | 2.451 | 0.9477 | 0.5579 | 0.6708 | 0.3363 | - | 1 | 153 | 28.92 |
| other | --transpose shift --roots spaced --merge-sustain 0.03 | 0.2 | 0.03 | 5 | 154 | 3 | 0.0499 | 6.646 | 71 | 0 | 0 | -13.02 | 0.4786 | -0.04053 | 0.2404 | 0.2338 | 1.775 | 0.9146 | 0.7235 | 0.5508 | 0.00497 | - | 0 | 0 | 0 |
| other | --transpose shift --roots spaced --merge-sustain 0.03 --max-shift 2 | 0.2 | 0.03 | 2 | 154 | 4 | 0.0661 | 6.646 | 71 | 0 | 0 | -11.96 | 0.4839 | -0.8072 | 0.2446 | 0.2551 | 1.407 | 0.9178 | 0.721 | 0.5775 | 0.08604 | - | 0 | 0 | 0 |
| other | --transpose shift --roots spaced --merge-sustain 0.06 | 0.2 | 0.06 | 5 | 74 | 4 | 0.1175 | 8.377 | 151 | 1 | 0 | 0.748 | 0.4504 | 0.03526 | 0.296 | 0.2531 | 1.418 | 0.8481 | 0.9525 | 0.4852 | 0.3276 | - | 0 | 0 | 0 |
| other | --transpose shift --roots spaced --merge-sustain 0.06 --max-shift 2 | 0.2 | 0.06 | 2 | 74 | 4 | 0.1095 | 1.561 | 151 | 0.5 | 0 | 0.9501 | 0.4382 | -1.884 | 0.2616 | 0.2231 | 1.456 | 0.8529 | 0.9375 | 0.4876 | 0.2867 | - | 0 | 0 | 0 |
| other | --transpose shift --roots spaced --merge-sustain 0.1 | 0.2 | 0.1 | 5 | 49 | 3 | 0.108 | 1.561 | 176 | 0 | 1 | -247.4 | 0.261 | -2.455 | 0.1071 | 0.08466 | 1.319 | 0.7088 | 1.301 | 0.05852 | 0.1482 | - | 0 | 0 | 0 |
| other | --transpose shift --roots spaced --merge-sustain 0.1 --max-shift 2 | 0.2 | 0.1 | 2 | 49 | 3 | 0.108 | 1.561 | 176 | 0 | 1 | -247.4 | 0.2656 | -2.447 | 0.1179 | 0.09862 | 1.348 | 0.767 | 1.262 | 0.03739 | 0.0932 | - | 0 | 0 | 0 |

## Iteration 3: joins that sum to the original, and glitch measures

`extract.py --xfade-ms X` makes each stored sample cover its segment plus X/2 each side (no baked fades except at the window's own edges), starts the event X/2 early and puts note-off X/2 before the segment end.
assemble.py then writes `attack_ms X release_lin_ms X release_s 0` for the part (sampler sp[2] and the new sp[16], a linear release that ends exactly X ms after note-off), so two adjacent pieces cross over X ms with gains that sum to 1.
The stem is first resampled to the engine's 48 kHz so every piece lands on whole output samples and plays at ratio 1; beats and lengths are written with 6 decimals.
`--share-null DB` replaces the spectral distance: two segments share a sample only if the shorter nulls against the other (best lag within 3 ms, one least-squares gain) below DB; pairs whose spectral cosine distance is over 0.4 are not tried.
`--no-share-rising` keeps a RISING sample (a swell: envelope peak after 50 percent of its length, first 30 ms more than 9 dB under the peak) for its own segment only.
`compare.py` gains a glitch section per stem (`glitch_*` rows; `overall` and every other number are unchanged): null residual after the best lag within 5 ms and one gain, dip and bump runs and seconds against the original's 8 ms envelope, level steps, clicks, residual periodicity, and with `--joins-drums|bass|other FILE` (one join time in seconds per line) the joins that dip within plus or minus 20 ms.
Tests: `python3 -I test_glitch.py`; the crossfade and sharing checks are in test_extract.sh.

## wa03: note-level recreation of one Lyria clip (cello and piano)

Pipeline (venv python): `wa03_nmf.py` (templates fitted to the CQT: piano decaying, cello sustained), `wa03_post.py` (exact onsets, pedal), `wa03_resid.py` (missing notes), `wa03_vel.py` and `wa03_timing.py` (closed loops against the original), `wa03_eqloop.py` (master EQ), `wa03_events.py` (notes to EVENTS), `wa03_build.sh` (render with `replica/wa03_play.c`, instrument as parameter), `wa03_iter.py` (scores and the iteration figure), `wa03_summary.py`, `wa03_results.py`.
Data and numbers: `replica/wa03/` (RESULTS.md, ITERATION_LOG.md, BUILD.md).
Tests: `python test_wa03.py` with the venv python.

Mutation checks of test_wa03.py (each break, the assertion that failed):

| test | mutation | assertion that failed |
|---|---|---|
| events | BPM 120 to 60 | beat must be exactly 2 * seconds |
| vel | velocity step sign flipped | the too quiet note must end above the right one: 0.542 0.350 |
| timing | move sign flipped | the late note must move earlier, got 1.0293 |
| resid | missing-note test inverted | the sounding C4 the replica lacks must be added at about 1.0 s |
| pedal | peak finder on the negated curve | pedal lifts at 5.50 7.64 (not near 4.0) |
