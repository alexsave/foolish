# Bot names

If Ultimate Tic-Tac-Toe ever gets bots, this is what they are called.
Nothing uses these names yet; the bots in `uttt/c/src/uttt_bots.c` only have working names (random, biro, roller, bias, nib, quill, fountain).

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

Chosen for even spacing from `make -C uttt/c ladder` (`tests/uttt_ladder.c`: each entrant is an engine at its own rollout budget, optionally with `~N` = N% of moves played at random; round robin, both colours on a shared seed, Elo fitted Bradley-Terry with random = 0; each entrant's CPU time a move is printed beside its Elo).

### Proposed, 2026-09-25: fountain is tier 7

The seven alone, 100 games a pairing:

| Tier | Name | Engine | Elo | Gap | ms/move |
|---|---|---|---|---|---|
| 1 | Freshman from Tempe | random | 0 | | 0 |
| 2 | Sophomore from Boulder | biro~60 (biro, 60% random moves) | 222 | 222 | 0 |
| 3 | Junior from Syracuse | biro | 489 | 267 | 0 |
| 4 | Senior from Davis | roller@25 | 663 | 174 | 0.3 |
| 5 | Graduate from Berkeley | roller@200 | 912 | 249 | 2.5 |
| 6 | Doctor from Oxford | quill@200 | 1331 | 434 | 4 |
| 7 | Professor from Cambridge | fountain@2500 | 1804 | 473 | 59 |

The top gap is the wide one, and at 98-100% scores the fit cannot resolve it finely anyway.
Owner decision 2026-09-25: tier 6 is quill@200 (was quill@60). Confirmed at 100 games a pairing: random 0, biro~60 222, biro 490, roller@25 662, roller@200 897, quill@200 1331, fountain@2500 1804. The top two gaps are the widest (434, 473) because nothing between quill@200 and fountain is worth a name.

### The old seven plus fountain, 100 games a pairing

| Engine | Elo | vs quill@4000 | ms/move |
|---|---|---|---|
| random | 0 | 0% | 0 |
| biro~60 | 221 | 0% | 0 |
| biro | 486 | 0% | 0 |
| roller@25 | 659 | 0% | 0.3 |
| roller@200 | 887 | 0% | 2.4 |
| quill@10 | 1047 | 2% | 1.0 |
| quill@4000 | 1602 | - | 65 |
| fountain@2500 | 1718 | 67% | 57 |

Head to head at equal CPU time (49.1 ms a move each), fountain@2500 scores 69% against quill@4000 over 400 games and 72% on another 400 (at 2600): about +150 Elo pooled. The log is in `uttt/c/README.md`.

Why it looks like this:
- Nothing real lives between random and biro: even one rollout a move beats random 97% of the time, so tier 2 is biro with deliberate random moves (the only engine here that needs the `~` mix; it is not in uttt_bots.c yet - add it as a noise parameter when bots ship).
- quill never saturated. The old table had quill@10 at 42% against quill@4000, and that was the ladder sharing quill's kept tree: quill@10 re-rooted the tree quill@4000 had just built and played its move. With a tree per side quill@4000 takes 98-100% off quill@10, so quill@4000 at 1602-1639 is not a tier-7 neighbour of quill@10 any more, and the old spacing (quill@10 1134, quill@4000 1278) is void.
- fountain@2500 costs about 50-60 ms a move on an M-series Mac, a little less than quill@4000, and is the strongest engine there is; quill@200 sits between roller@200 and it.
- Left out: crn and bias (single-idea experiments), nib and sniper (between tiers 5 and 6), quill@4000 (116 Elo under fountain for more time).

The names are UI strings, so they belong in the C string table (`uttt_say`) like every other word in this game, and are localized there if the game ever is.
