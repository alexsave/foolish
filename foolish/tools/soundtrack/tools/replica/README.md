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
