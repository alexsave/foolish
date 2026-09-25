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

Which engine fills which tier is decided then, from a fresh arena run.
The current strength order is random < biro < roller < bias ~ nib < quill, which is six, so one tier needs a new engine or a split budget (quill at two rollout counts).
The names are UI strings, so they belong in the C string table (`uttt_say`) like every other word in this game, and are localized there if the game ever is.
