# Hypotheses for the throwaway Graviton run

Written 2026-11-09 before measuring, for the run at the end of the memory series.
Each has a prediction, the reason, and what refutes it.
Hardware: a fresh t4g.nano server (unlimited credits) and a c7g load box, production static musl builds, interleaved base and change runs.

## Carried over from Docker (foolish #480, roster)

- G1: per resident game, RssAnon drops by about 3,100 B (Docker: 3,120 B). Refuted if under 2,400 B.
- G2: sat2 RSS p50 drops by about 12.5 MiB (Docker: 46.9 to 34.4). Refuted if under 9 MiB.
- G3: sat2 moves/s, move p50 and p99 are no worse than the base band of 3 runs. Refuted if any is worse than the band's width.
- G4: the series' later PRs (state buffer, replay scratch) keep their Docker verdicts on the nano, as they will state them.

## Never measured so far

- G5: musl idle RSS on the nano is 6.5 to 7.5 MiB, of which 1.7 to 2.2 MiB is private dirty (Docker: 6.9 and 1.9). Refuted outside those ranges.
- G6: the nano's transparent huge pages setting is "madvise" or "never" (AL2023 default), so huge pages do not inflate RSS. Refuted if it is "always" and idle RSS exceeds 9 MiB.
- G7: sustained throughput once the nano's CPU credits are spent (standard credits, not unlimited) is 2,000 to 3,500 moves/s at sat2, about a tenth of burst, because the baseline is 5% of 2 vCPUs. Refuted outside that range.
- G8: two different apps' static binaries share no code pages, but two copies of the same binary share their text: a second instance of the same binary adds at most its private dirty plus 0.5 MiB to the box's used memory. Refuted if it adds more than 3 MiB.
- G9: a bot-heavy load (every table with bots) makes job-0 touch most of the solver scratch it uses, between 0.4 and 1.6 MiB of its 1.69 MB TLS block, and no other thread grows its TLS beyond 64 KiB. Refuted if job-0 touches under 0.2 MiB or another thread over 64 KiB.
- G10: legal_any (human moves) touches none of the bot scratch: under a humans-only load, req-0's and job-0's touched TLS stays under 64 KiB. Refuted above that.
- G11: a rematch after its game is evicted from the cache adds under 0.5 ms to that one action against a cached rematch. Refuted above 2 ms.
