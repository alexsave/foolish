# Transcription tools

Stem separation and note transcription of reference audio, and a per-window report a composer can re-score from.
All outputs are measurements; nothing here describes how audio sounds.

## Files

- `stems.sh FILE OUTDIR` runs demucs (htdemucs, CPU) and writes `OUTDIR/htdemucs/<name>/{drums,bass,other,vocals}.wav`.
- `notes.sh FILE OUTDIR [LABEL]` runs basic-pitch and writes `<label>_basic_pitch.csv` and `.mid`.
- `window_report.py` writes the per-window Markdown and a raw-note TSV from the stems, the note CSVs and a bar grid.
- `envelopes.py` measures attack, peak to sustain, sustain level, release and spectral centroid per note on one mono stem.
- `test_transcribe.py` is the deterministic end-to-end test.

## Environment

The venv was made with `uv venv --python 3.11` and then `uv pip install basic-pitch demucs onnxruntime 'setuptools<81' --exclude-newer 2026-10-31`.
The default location is `/Users/alex/Dev/soundtrack-tools/venv`, overridable with `SOUNDTRACK_VENV`.
Model weights live in a `TORCH_HOME` folder outside the repo (default `/Users/alex/Dev/soundtrack-tools/torch_home`).
Run the Python tools with the venv python, for example `/Users/alex/Dev/soundtrack-tools/venv/bin/python test_transcribe.py`.

## Known limits

- basic-pitch never emits notes shorter than about 128 ms, so the share of notes under 60 ms is 0.000 on every real file.
- Bar grids come from the earlier DSP report; outside its windows the report lays bars out at the section tempo with unknown phase.
- Drum classes come from spectral-flux onsets and band power gain over the 65 ms after each onset.
- The envelope test uses the ground-truth note list, because the transcribed note end is a model output.

## Test and mutation checks

The test prints one PASS or FAIL line per named check.
Each mutation below was run on a scratch copy of this folder (input mutations through `TRANSCRIBE_MUTATE=<name>`, code mutations by editing the copy), and the checks that failed are listed.
The unmutated test passes all 11 checks.

| mutation | failed checks |
|---|---|
| expected pitches shifted by 1 semitone | notes_bass_recall, notes_melody_recall |
| expected onsets shifted by 100 ms | notes_bass_recall, notes_melody_recall |
| synthesised attack 0.03 s to 0.08 s, expectation unchanged | envelope_attack_s (also both recall checks, the slower attack delays the transcribed onsets) |
| synthesised sustain 0.5 to 0.25, expectation unchanged | envelope_sustain_db |
| synthesised release 0.2 s to 0.28 s, expectation unchanged | envelope_release_s |
| kick moved from step 8 to step 4 | report_drum_low |
| attack upper threshold 0.9 to 0.5 of peak | envelope_attack_s |
| release threshold -20 dB to -6 dB | envelope_release_s |
| peak-to-sustain band 1 dB to 6 dB | envelope_peak_to_sus_s |
| sustain measured on the first tenth instead of the middle half | envelope_peak_to_sus_s, envelope_sustain_db |
| centroid windows swapped | envelope_centroid_falls |
| minor triad template made major | report_chords |
| drum step index shifted by one step | report_drum_low, report_drum_high |
| bass names printed one semitone high | report_bass_names |
| notes.sh passes `--minimum-frequency 2000` | notes_bass_recall, notes_melody_recall, report_bass_names, report_chords |

Two first attempts survived and were strengthened: attack threshold 0.9 to 0.8 and release threshold -20 dB to -10 dB both stayed inside the 30 percent tolerance, so the mutations were made larger (0.5 and -6 dB) and then failed as listed.
