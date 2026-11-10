# The server memory series: measurements and harness

The measurements behind foolish #480, #481 and #483 (roster compaction, the journal's state buffer, the bot memory block), taken on 2026-11-09.
The server's resident memory (RSS p50, saturated) went from 46.9 to 21.2 MiB on a musl build, and from 40.8 to 21.3 MiB on a real t4g.nano.

## Read in this order

- `RESULTS.md`: cbase serve's memory on a real t4g.nano, the first run.
- `MUSL_IDLE.md`: the idle breakdown of the musl static build.
- `VERIFY.md`: verification of the attributions in RESULTS.md.
- `HYPOTHESES.md`: the predictions, written before the final run.
- `GRAVITON.md`: the final confirmation run, four builds interleaved (A before the series, D after #483).

## What is here

`harness/` holds the small files the documents cite: the Graviton run's scripts (`ec2/`, `grav/`), the mutation runner (`mutate.py`), and the microbenchmark and gate sources (`mem2_jbench.c`, `mem3_jbench.c`, `mem3_gate.c`).
The scripts name the paths of the original scratch folder, `~/Dev/cdom-memtest`.

## What is not here

The raw data was not kept: the roster, mem2, mem3, verify and grav result folders (about 11 GB, mostly build outputs and per-sample captures).
Every number the documents rely on is in their tables.
Rerunning needs a throwaway t4g.nano and a load box, as GRAVITON.md's Setup describes; tag the resources so teardown can be verified, and upload the load clients as one tarball.
