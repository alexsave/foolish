# Bot names

If Ultimate Tic-Tac-Toe ever gets bots, this is what they are called.
Nothing uses these names yet; the bots in `uttt/c/src/uttt_bots.c` only have working names (random, biro, roller, bias, nib, quill).

The pattern is foolish's (`docs/IOS_BOT_NAMING.md`): a fixed ladder of seven difficulty tiers, weakest first, each with one name.
foolish climbs cities on the road to Moscow; this game climbs a university career, in college towns.

## The two lists

Cities, weakest to strongest:

1. Tempe
2. Boulder
3. Syracuse
4. Davis
5. Berkeley
6. Oxford
7. Cambridge

Titles, weakest to strongest:

1. Freshman
2. Sophomore
3. Junior
4. Senior
5. Graduate
6. Doctor
7. Professor

## The ladder

A bot's name is its title and its town, in the same tier.

| Tier | Name |
|---|---|
| 1 | Freshman from Tempe |
| 2 | Sophomore from Boulder |
| 3 | Junior from Syracuse |
| 4 | Senior from Davis |
| 5 | Graduate from Berkeley |
| 6 | Doctor from Oxford |
| 7 | Professor from Cambridge |

## The tier list

Chosen for even spacing from `make -C uttt/c ladder` (`tests/uttt_ladder.c`: each entrant is an engine at its own rollout budget, optionally with `~N` = N% of moves played at random; round robin, both colours on a shared seed, Elo fitted Bradley-Terry with random = 0).
Final check, 2026-09-24: the seven alone, 100 games a pairing.

| Tier | Name | Engine | Elo | Gap |
|---|---|---|---|---|
| 1 | Freshman from Tempe | random | 0 | |
| 2 | Sophomore from Boulder | biro~60 (biro, 60% random moves) | 223 | 223 |
| 3 | Junior from Syracuse | biro | 491 | 268 |
| 4 | Senior from Davis | roller@25 | 667 | 176 |
| 5 | Graduate from Berkeley | roller@200 | 894 | 227 |
| 6 | Doctor from Oxford | quill@10 | 1134 | 240 |
| 7 | Professor from Cambridge | quill@4000 | 1278 | 144 |

Each tier beats the one below it 74-84% of the time, except the top pair (58%).

Why it looks like this:
- Nothing real lives between random and biro: even one rollout a move beats random 97% of the time, so tier 2 is biro with deliberate random moves (the only engine here that needs the `~` mix; it is not in uttt_bots.c yet - add it as a noise parameter when bots ship).
- quill saturates: 10 rollouts to 4000 is only ~150 Elo because its root already carries sniper's proof and the heuristic, so the top gap is the narrowest there is. A stronger tier 7 needs a better engine, not more rollouts.
- Left out: crn and bias (single-idea experiments), nib and sniper (they sit at ~960-1010, between tiers 5 and 6, and would crowd them), quill at 25-1000 (inside the top gap).

The names are UI strings, so they belong in the C string table (`uttt_say`) like every other word in this game, and are localized there if the game ever is.
