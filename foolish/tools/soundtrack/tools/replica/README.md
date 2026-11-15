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
