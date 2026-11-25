# t4g.nano server performance, November 2026

The record of the benchmarking and optimization campaign on foolish's server, 2026-11-11 to 2026-11-14.
The baseline was foolish `9c5057b64`.
The code that came out of it is merged: PRs #496, #497, #498, #500 and #509.

## Where to read

- `campaign/NOTES.md`: the campaign's notes, ranked plan and results, trip by trip. Start here.
- `campaign/trip*/out/TRIP*_RESULTS.md`: each AWS trip in full.
- `FABLE_HANDOFF.md`: the findings before the campaign. Read its correction first: the 24k and 38k moves/s figures are two different test shapes.
- `GAP_38K_VS_24K.md`: the investigation behind that correction.
- `load-results.md`: the same load on the M1 Mac.
- `cachesim/SUMMARY.md`: the valgrind cache model.

## What was not built

The end of the Trip 3 section in `campaign/NOTES.md` lists what is left on the loop thread, with its cost per move.

## What was not kept

These files are a record, copied as they were, except as listed here.

- Raw run output (every `raw/` directory), load archives, perf data and all binaries were deleted.
  A document that points at one of them points at something that no longer exists.
- Copies of `journal.c` used by `campaign/listbench` are `foolish/server/src/store/journal.c` at `59c115054` (A) and `9c5057b64` (B).
  `campaign/listbench/prep.sh` shows the edits made to them.
- `grav/build` (a source snapshot), the large cache model dumps and the AWS run state files were deleted.
- Files whose bytes are already here or in `../harness` are listed in `DUPLICATES.txt`.
- AWS instance, security group, subnet and VPC ids are replaced by `*-REDACTED*`.
- In the `.md` files, the old results folder's path is rewritten to this directory.

The scripts still hold the absolute paths of the machine and worktrees they ran on, and `scripts/sat2_run.sh` read an environment file under `/tmp` that is gone.
Fix those before running one again.
