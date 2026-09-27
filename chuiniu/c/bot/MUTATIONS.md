# cn_bot_test mutation checks

Every test in `cn_bot_test.c` was broken on purpose by hand, run with `make bot-test`, seen to go red on the named assertion, and restored.
After the last restore `git diff` on `cn_bot.c` was empty and the suite was green (210,779 checks, 0 failures).

| # | Test | Break (in `cn_bot.c`) | Red assertion |
|---|------|-----------------------|---------------|
| 1 | binomial table is exact | `binom_init`: the pmf recurrence ratio `* 0.5` became `* 0.51` | `pmf(2,1)` 0.4533 vs 4/9, `tail(2,1)`, `tail(30,10)`, `tail(30,20)` |
| 2 | claim primitive, closed form | `cn_binom_tail_p`: the sum `i >= m` became `i > m` | `5/9` came out 0.1111; the owner's case at p = 1/6 came out 1.05e-4 and at p = 1/3 came out 0.0376 |
| 3 | bid likelihood is the softmax over every legal option | `lik_fast`: the call term dropped from the normaliser (`if (pq > 0)` became `if (0)`) | fast vs brute force, e.g. 0.3054 vs 0.2212 |
| 4 | belief moves the right way | `cn_belief_build`: a seat's bids applied to every other seat (`x->seat != seat` became `==`) | seat 1's 3s did not rise (1.640); seat 2, who never bid, left the prior |
| 5 | sampling respects the belief | `cn_belief_sample`: the draw `u` became `u * u` | face 2 k 0 sampled 0.409 against belief 0.166 |
| 6 | belief claim is the exact convolution | `cn_belief_claim`: the per-seat loop `j <= n` became `j < n` | at the prior it no longer matched the binomial, q 1 onward |
| 7 | fitted temperature reads bluffers | `cn_fit_beta`: the arg max became an arg min | honest bidder fitted 0, bluffer fitted 32 |
| 8 | rollouts are deterministic and paired (determinism) | `eval_worlds`: the world stream seeded `seed + ctr++` from a thread-local counter | "candidate 1 repeats exactly" |
| 9 | rollouts are deterministic and paired (common random numbers) | `eval_worlds`: each candidate's rollout stream offset by its index (`ro_rng + c`) | "a duplicate candidate: 0.7396 vs 0.7500" and "order does not change an estimate" |
| 10 | rollouts are deterministic and paired (exact call) | `eval_worlds`: the call scored by rollouts instead of the closed form | "call value 0.000000" against 1 - 0.9947 |
| 11 | obvious decisions | `cn_bot_choose`: the arg max became an arg min | "calls an impossible bid, got (10, 6)" and "does not call a certain bid" |
| 12 | whole games | `cn_seen`: my dice copied from the next seat's row | "my own dice" |
