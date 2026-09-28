# Chui Niu - the offline bot

A first-pass Liar's Dice bot, in C, for simulation and evaluation only.
It is not in the iOS app: no lobby seat, no bridge entry point, nothing under `chuiniu/ios/`.
The code is `chuiniu/c/bot/`; the decisions that matter are B1 to B4 in `DECISIONS.md`.

## Files and commands

- `cn_bot.h`, `cn_bot.c`: what a seat saw, the binomial and the claim primitive, the opponent model, the belief, the rollouts, the decision.
- `cn_bot_random.c`: the baseline, uniform over the kernel's legal options.
- `cn_arena.c`: whole games between any mix of policies.
- `cn_bot_test.c`: the tests; `MUTATIONS.md` records the break that turned each one red.
- `Makefile`: its own, compiling the kernel's sources by relative path, so it never touches `chuiniu/c/Makefile`.

```
make -C chuiniu/c/bot bot-test     # 210,779 checks, 0 failures
make -C chuiniu/c/bot asan         # the same under ASan + UBSan
make -C chuiniu/c/bot arena        # bot vs random, 4000 games at 2 and at 4 seats
make -C chuiniu/c/bot arena-fast   # a smoke run of the same
chuiniu/c/bot/build/cn_arena --seats=4 --mix=bot,prior,prior,prior --games=4000 [--seed=1 --threads=8 --worlds=96 --fast]
```

## What the bot may read

The bot decides from a `CnSeen`, never from a `CnGame`.
`cn_seen()` is the one function that looks inside the kernel's struct.
It replays the history through `cn_new` / `cn_apply` and copies out only what the seat saw at the table: its own dice, every seat's dice count, every bid with the seat that made it and the bid it raised, and the hands shown at every past call.
So the rest of the bot cannot cheat by accident: another seat's current dice are not in its input.

## The claim primitive (closed form)

With 1s wild each unseen die counts toward a named face with probability p = 1/3.
For a bid of q, my hand of d dice with k counting, and `total` dice on the table, the bid is true when X >= max(0, q - k), X ~ Binomial(total - d, p).
`cn_binom_tail_p(U, m, p)` is the direct sum over i = m..U of C(U, i) p^i (1 - p)^(U - i), in doubles, for any p, with the terms built by the ratio recurrence and summed from the top down.
`cn_claim_prob(total, d, k, q, p)` is that question in the bid's terms, and `cn_binom_tail` is the same sum at p = 1/3, tabled.

The owner's worked case (25 dice, I hold 5 with k = 2, the bid is 12, so m = 10 of U = 20) comes out, exactly:

- p = 1/6 (no wilds): 5.985e-4.
- p = 1/3 (our rules): 0.0919, about 150 times larger.

The p = 1/6 value is small, but it is not "about 1e-5 or below" as the brief expected.
The "4 standard deviations" estimate assumes a normal tail; a binomial's upper tail that far out with p = 1/6 is much fatter.
The value was checked against Python's exact `fractions` sum, and the test asserts it to 1e-16, together with the exact small cases (U = 2, p = 1/3, m = 1 is 5/9; U = 3, p = 1/2, m = 2 is 1/2; U = 4, p = 1/6, m = 4 is 1/1296).

When the belief refines p per seat, the flat binomial no longer applies.
Each seat's belief is a full distribution over how many of its dice count toward each face (0..n), not a binomial with a refined p, so the refined question is the exact convolution of the other live seats' count distributions, `cn_belief_claim`.
This needs no Poisson-binomial: the convolution of at most five distributions of length at most six is exact and cheap.
At the prior it equals `cn_claim_prob` at p = 1/3 (a test holds it to that).

The closed form answers every "is this claim plausible right now" question the bot asks: the call's value, the candidate list, and every judgement inside a rollout.
Monte Carlo remains only for the multi-turn question, what happens after I raise.

## The opponent model

### How a seat is assumed to bid

A seat on turn holding hand h, facing standing bid b with `total` dice on the table, picks each legal option x with probability

    exp(beta * u(x)) / Z(h)
    u(raise (q, f)) = P(claim true | h)  = tail(total - n, q - k_h(f))
    u(call)         = P(b is false | h)  = 1 - u(raise b)

and Z(h) sums exp(beta * u) over every legal option: the call when allowed and every legal raise, up to 150 of them.
This is a quantal response: a seat mostly makes claims its own hand supports, and beta is the bluff temperature (0 is a random player, large is a seat that never bluffs).
The seat judges the unseen dice by the prior; it does not model what the others know (level 0).

Why this form rather than a hand-made "evidence weight per bid":

- A raise on the same face at a high quantity is strong evidence, because u falls steeply in q - k_h(f) there.
- A low bid that everyone could make (two 6s among 15 dice) is almost no evidence, because u is near 1 for every hand; the test "my own 6s bid" had to use four 6s for this reason.
- The normaliser Z(h) supplies the "why this face and not another" evidence: a hand strong in 5s that bid 3s is penalised, because it had better options.
- Raising instead of calling is evidence too: the call is one of the options, so a hand that makes the standing bid look false is less likely to have raised.

Computing Z(h) naively is 150 exponentials per hand per bid.
Since u of a raise depends only on need = q - k_h(f), a table of exp(beta * tail(m, need)) and its running sum make Z(h) five differences plus the call term, O(1) per hand (`lik_fast`); a test checks it against the brute-force sum over every legal option to 1e-12.

### The belief

For each seat, the belief is the exact posterior over its hand as a multiset of faces: C(n + 5, 5) count vectors, 252 at five dice, each with its multinomial prior, times the likelihood of every bid that seat made this round.
The brief sketched a per-seat per-face weight table; I kept that table as the belief's marginal (`pk[seat][face][k]`, what the rollouts and the claim primitive read) but made the posterior itself the joint over the hand.
The reason is the wild 1s: one die showing 1 counts toward every face, so the faces of one hand are strongly coupled, and a per-face table updated face by face would double-count every 1.
252 entries per seat is still small, and sampling a hand is one binary search in its running sum.

Seats are independent given their own bids (my dice and theirs are independent rolls, and a seat's bid depends on its hand, not mine), so the joint over the table is the product of the per-seat posteriors.
The belief is rebuilt from the `CnSeen` on every decision; it carries no state between calls.
Every seat gets one, including me: the others' view of me is what a rollout opponent would read.

### The temperature, and why it is fixed

beta is 8 for every seat.
`cn_fit_beta` can fit it per seat instead: at each past call every hand was shown, so each of the seat's old bids can be scored against its true hand under each beta in {0, 2, 4, 8, 16, 32}, with a one-nat bonus for the default.
The test proves the fit tells a bluffer (fitted 2 or less) from an honest bidder (8 or more).
In play it lost: against the bot family it fitted about 4 most of the time, and `bot` (fixed 8) beat `fitbeta` 0.555 [0.540, 0.570] at two seats.
The softmax is a misspecified model of these bots (their choices come from rollouts, not a softmax), so the maximum-likelihood beta is not the most useful one for reading them.
The fit stays in the code, off, with the measurement.

## Sampling and common random numbers

For each of `worlds` (96) sampled worlds, every other live seat's hand is drawn from its posterior; mine is my real hand.
World w's stream and its rollout stream both come from splitmix64 seeded by the decision's seed, so a decision is a pure function of its input and its seed.
Every candidate is scored on the same 96 worlds with the same rollout stream (common random numbers), so the comparison between candidates is paired: a duplicated candidate, or the same candidate in another position, gets the identical estimate (a test holds it to exact equality).

## The rollout

A rollout is a light round, not a `CnGame`: the standing bid, whose turn, each seat's counting dice in the sampled world, and the public marginals.
It starts with my candidate and plays to the end of the round (the call), and its value is 1 when I keep my die.
To the end of the round, not the game: a round's die is the whole stake of a decision, the next round is a fresh roll, and whole-game rollouts would multiply the cost by the number of rounds left for a signal that is mostly noise.

The rollout policy is the same softmax the belief assumes, over a short list (the call, and on each face its least legal quantity and one more), judged by the acting seat's own sampled dice through the closed-form claim (`pub_true`, the same convolution as `cn_belief_claim`).
So the rollout opponents play the way the bot believes they play, and they are not fools: they call implausible bids and raise on their strong faces.

### Who reads whose bids: the one finding that mattered

The first version let everybody in a rollout read everybody's rollout bids (each bid updated the bidder's public marginal).
It lost badly to the bot with the opponent model switched off.
The reason: in its rollouts, opponents believed its bluffs and so rarely called them, and its real opponent did not read bids at all, so the bluffs got called.

The fix is to make the rollouts level 1, consistent with the likelihood above (which models a bidder as judging by the prior):

- Rollout opponents judge by their own dice and the prior, and read nobody's bids (`opp_reads = 0`).
- I read the rollout opponents' bids with the same model as the real ones (`observe = 2`), through a cheap update of that seat's face marginal by exp(beta * tail(m, q - k)), without the normaliser (the full joint update is too slow for thousands of rollout steps; the normaliser's "why this face" part is second-order).

The arena numbers for each reading are in the evaluation below.

## The decision

The candidates are: the call (when allowed), and for each face its least legal quantity, one more, and the highest quantity whose closed-form belief claim is still at least 0.5 (`CN_RAISE_SAFE`), when that is higher still.
That is at most 16.
The call's value is exact, `1 - cn_belief_claim` for the standing bid: it ends the round now, so it needs no rollout, and its closed form is the expectation of what its rollouts would say.
Each raise's value is the mean over the 96 worlds of its rollout.
The bot plays the highest value; ties go to the earlier candidate (the call first).

## The constants

All in `cn_bot_cfg_default` and the two `#define`s above it in `cn_bot.c`:

| Constant | Value | Meaning |
|----------|-------|---------|
| `worlds` | 96 | sampled worlds per decision |
| `beta` | 8 | bluff temperature of every seat in the belief |
| `ro_beta` | 8 | the rollout policy's temperature |
| `fit_beta` | 0 | per-seat temperature fit, off (measured worse) |
| `use_belief` | 1 | the posterior from bids; 0 is the prior |
| `observe` | 2 | in rollouts I read the others' bids, nobody reads mine |
| `opp_reads` | 0 | rollout opponents judge by the prior |
| `rollout_steps` | 64 | a cap on moves per rollout (a round ends sooner) |
| `CN_RAISE_SAFE` | 0.5 | the "comfortable" raise candidate's threshold |
| `CN_FIT_PRIOR_NATS` | 1.0 | the fit's bonus for the default temperature |

## Evaluation

Every run: `--seed=1`, 96 worlds, 8 threads (a run is identical at any thread count), seats rotated so each policy sits in every position equally often.
Win rates carry a 95% Wilson interval; dice lost is per seat per game, with a 95% normal interval on the Bessel-corrected standard error (B5).

### The baseline: uniform over the legal options

| Seats | Mix | Games | Bot wins | Bot win rate [95% CI] | Fair share | Bot dice lost |
|-------|-----|-------|----------|-----------------------|------------|---------------|
| 2 | bot, random | 4000 | 4000 | 1.000 [0.999, 1.000] | 0.500 | 0.128 [0.116, 0.139] |
| 4 | bot, random x3 | 4000 | 3998 | 1.000 [0.998, 1.000] | 0.250 | 0.318 [0.299, 0.336] |
| 4 | bot x2, random x2 | 4000 | 4000 of 4000 games | 0.500 per bot seat | 0.250 | 3.496 |

The intervals separate completely, and they say little: a uniform player raises to arbitrary quantities (most legal raises are near the top) and almost never calls, so any player who calls implausible bids beats it.
The random baseline is a sanity floor, not a measure of the opponent model.

### What the opponent model is worth: the bot's own ablations

Each ablation is one switch away from `bot`; this is where the design choices above were decided.

| Seats | Mix | Games | bot win rate [95% CI] | Fair share |
|-------|-----|-------|-----------------------|------------|
| 2 | bot, prior | 4000 | 0.748 [0.735, 0.761] | 0.500 |
| 2 | bot, noread | 4000 | 0.520 [0.505, 0.535] | 0.500 |
| 2 | bot, readers | 4000 | 0.514 [0.498, 0.529] | 0.500 |
| 2 | bot, fitbeta | 4000 | 0.555 [0.540, 0.570] | 0.500 |
| 2 | bot, bot | 4000 | 0.500 [0.489, 0.511] | 0.500 |
| 4 | bot, prior x3 | 4000 | 0.294 [0.280, 0.308] | 0.250 |
| 4 | bot, prior, bot, prior | 4000 | 0.292 [0.282, 0.302] | 0.250 |

- `prior` is the same bot with the opponent model off: every hidden hand from the prior, no bid updates anything. The belief bot beats it three games in four at two seats and takes 18% more than its fair share at four.
- `noread` keeps the decision-time belief but reads no rollout bids: most of the gain is the decision-time belief; reading rollout bids adds a small edge (0.520, the interval just clears 0.5).
- `readers` is the first version (everybody reads everybody in rollouts). Level 1 edges it head to head, but the real difference is against `prior`: `readers` wins only 0.066 [0.058, 0.074] of 4000 two-seat games against it (`--mix=readers,prior`), because its bluffs are built for opponents who believe them.
- `fitbeta` is the fitted temperature, discussed above.

The four-seat edge is smaller than the two-seat one.
With more seats a round's die usually goes to someone else whatever I do, so a decision moves my value less, and the naive prior is a better approximation of three hands than of one.

## Limits, and what a second pass would try

- The belief uses this round's bids only; across rounds it learns nothing (the temperature fit was the attempt, and it lost).
- The bidder model is level 0 (it judges by the prior), and the rollouts are level 1; a seat that reads my bids is not modelled.
- The in-rollout update drops the normaliser.
- The value is "I keep my die this round": it ignores that losing my last die ends my game, and it ignores who else loses.
- No bid on 1s and no "spot on" (the rules have neither, R2).

## Kernel API notes

Nothing blocked; the bot uses only `cn.h` (`cn_new`, `cn_apply`, `cn_legal`, `cn_rank`, the `CnGame` fields).
Two things a bot needs that the kernel does not export, written on the bot's side:

- The seat that made each move and the hands shown at every past call: `cn_seen` rebuilds them by replaying the history (the kernel keeps only the newest call's `shown`).
- A count of a face "including wild 1s" over a chosen set of dice: `cn_count` counts the whole table from `g->dice`, which a bot must not read, so the bot counts its own and sampled hands itself (`cn_hand_k`).
