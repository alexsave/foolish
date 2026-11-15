# ALT-CELLO measurement suite

Run everything with `nice -n 15 /Users/alex/Dev/soundtrack-tools/venv/bin/python -I <script>`.

- `measure.py`: per-note and median measurements (attack, release, harmonics, LTAS and peaks, noise residual, micro-variation, vibrato, anti-synthy).
  CLI: `measure.py --wav X.wav --notes NOTES.txt --label NAME --out PREFIX` writes PREFIX.tsv, PREFIX.md, PREFIX_notes.tsv, PREFIX_hseries.tsv.
  NOTES.txt lines: `start_s dur_s midi`.
  Chart: `measure.compare_figure(alt_prefix, target_tsv, title, out_png)` with target_tsv = `/Users/alex/Music/foolish_cities_v5/alt_cello/targets/TARGETS.tsv`.
- `targets.py`: measures the benchmark cellos and writes the TARGETS files (read only on the benchmarks).
- `test_measure.py`: synthetic signals with known answers.

## Mutation checks

Each test was made to fail by breaking the measured code, then restored:

- test_vibrato: vibrato depth halved in `vib_analysis` -> fails "10.05 not less than 3.0" (depth check). Restored, green.
- test_harmonic_slope: `h%d` read from the wrong harmonic row -> fails "6.02 not less than 1.0" (h2 = -6 dB). Restored, green.
- test_hnr (autocorrelation): `20 log10` instead of `10 log10` in `autocorr_hnr` -> fails "autocorrelation HNR at 10".
- test_hnr (residual): residual taken as `body - 0.5 * harm` -> fails "7.3 not less than 2.0 : residual HNR at 10". (A weaker mutation, `tot - 2N`, did not fail; it only moves 0.5 dB.)
- test_attack: 90 percent point replaced by 50 percent -> fails "50.0 not less than 20.0".
- test_inharmonic: stiffness fit forced to B = 0 -> partial 10 is not found, fails "nan not less than 2.0".
- test_cli_and_compare_figure: `savefig` removed from `compare_figure` -> FileNotFoundError for the PNG.
  The test also caught a real bug (`read_tsv` crashing on the empty spread columns of TARGET rows), fixed.
