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

## Open when bots ship

Which engine fills which tier is decided then, from a fresh ladder run (`make -C uttt/c ladder`, `tests/uttt_ladder.c`: each entrant is a bot at its own rollout budget, Elo fitted Bradley-Terry to the round robin, random = 0).

First run, 2026-09-24, 60 games a pairing, 111s on 8 cores:

| Tier | Name | Entrant | Elo |
|---|---|---|---|
| 1 | Freshman from Tempe | random | 0 |
| 2 | Sophomore from Boulder | biro | 420 |
| 3 | Junior from Syracuse | roller@200 | 788 |
| 4 | Senior from Davis | nib@200 | 924 |
| 5 | Graduate from Berkeley | quill@200 | 1327 |
| 6 | Doctor from Oxford | quill@1000 | 1391 |
| 7 | Professor from Cambridge | quill@4000 | 1420 |

More rollouts help quill less and less: 200 to 4000 is only +93, and quill@4000 beats quill@200 just 54% of the time.
So the top three tiers are close, and the biggest gap is between nib and quill (+400).
If the ladder should feel even, try quill at a very low budget (e.g. 25-50) for tier 5 and re-run.
The names are UI strings, so they belong in the C string table (`uttt_say`) like every other word in this game, and are localized there if the game ever is.
