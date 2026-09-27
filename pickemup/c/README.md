# The kernel, and nothing but the rules

```
make -C pickemup/c run        every test: rules, masking, plan, words, 2,800 fuzz games
make -C pickemup/c asan       the same under ASan + UBSan (2,000 fuzz games)
make -C pickemup/c wasm       the kernel as wasm32 objects, freestanding (needs a wasm clang)
./pickemup/c/build/pk_fuzz 50000 7     more games, another stream
```

`pk.c` is the rules of `pickemup/docs/RULES_AND_KERNEL.md` and nothing else: the deal, legality, apply, seal and undo.
Every rule a host could ask about is answered here, so a host never re-decides one.
The struct is fixed-size plain integers with no pointers and no bitfields, there is no allocation, and the only libc it reaches is `memcpy`, `memset`, `strlen` and `strncmp`, so the same files build for wasm32 and into an xcframework.

## The file map

| File | What |
|---|---|
| `src/pk.h`, `src/pk.c` | the rules: cards, state, deal, legality, apply, seal, undo by replay, the hash |
| `src/pk_deck.c` | card ids 0..103 and the one Fisher-Yates, keyed from `shared/c/deal_rng` |
| `src/pk_plan.h`, `src/pk_plan.c` | the animation plan: events per bubble, the draft's own, "since last seen" |
| `src/pk_view.h`, `src/pk_view.c` | the masked per-seat view |
| `src/pk_lobby.h`, `src/pk_lobby.c` | the lobby's verdicts, join, leave, start |
| `src/pk_say.h`, `src/pk_say.c` | which sentence a position says, composed by key |
| `src/pk_internal.h` | the sink `pk.c` shares with `pk_plan.c` and the tests |
| `i18n/keys.h`, `i18n/strings_en.c` | every word, one key list, in the shape `shared/tools/datagen` reads |
| `tests/pk_check.h` | the harness: `CHECK`, hand-built tables, the random bot |
| `tests/pk_test.c` | legality (7.1), effects (7.2), the deck (7.3), call-out windows (7.7), lobby and edges (7.8) |
| `tests/pk_plan_test.c` | masking (7.5) and the plan (7.6) |
| `tests/pk_say_test.c` | the words (6) |
| `tests/pk_fuzz.c` | random play at 2..8 players against every invariant |
| `tests/MUTATIONS.md` | the mutation each test was seen to fail on |

Still to come, beside these and without changing `pk.h`: the coder and the envelope (`pk_code`, `pk_msg`, section 4 of the design), the iOS bridge (`ios/pk_api.c`, `ios-lib`, `ios-smoke`) and the CI lane.
The envelope reaches the game only through `pk_new`, `pk_legal_turn`, `pk_apply`, `pk_seal` and the read-only fields; its seat verdicts are `pk_lobby.h`'s.

## How it hangs together

**One apply path.**
`pk_apply`, `pk_seal` and `pk_new` run through static functions that take an optional sink.
With no sink it is play; with one it is the animation plan.
`pk_plan` replays the game from its seed through that one path and reports what it does as it does it, so an event list cannot disagree with the state it leads to.

**Undo is a replay.**
`hist[]` holds every choice: a `BUBBLE` record per bubble (sender, "Last card!", the catch, sealed), the turn actions, and a `CONTINUE` where a two-player sender took the next turn too.
`pk_undo`, `pk_unsay`, `pk_uncall` and `pk_to_floor` rebuild the game from the seed through a shorter or edited history, and refuse to go below the draft's floor, which sits after its last draw (D8).
`pk_replay` also checks that the history it rebuilt is record for record the one it was given, which is what the envelope's decode will lean on.

**Bubble-level things belong to the bubble.**
"Last card!" and "Caught you!" live in the bubble's record, not at a moment in it, and a replay applies them at the bubble's open.
Nothing a turn action does reads them, and the catch is judged against the table as it stood at open and dealt at seal (D5d), so the order they were tapped in never matters.

**Counts never reach a screen.**
`PkView` has no slot for another seat's count while the game is played: every other seat's fan is drawn at `PK_FAN_BACKS` backs, and `reveal_n` is filled only once the game is over.

**The words are keys.**
`pk_say.c` never formats a user string: every sentence is a key filled by `pk_fill`, numbers go through `pk_itoa`, and a caption is built from the bubble's own events (`pk_say_caption_of`), so a host holding a plan can caption it without a second replay.

## Measured

Over the 2,800 fuzz games of `make run` (400 at each table size): about 184 turn actions and 134 bubbles a game, the longest 1,051 actions, about 3,000 reshuffles in all, and two games stopped by the 750-bubble cap.
The whole suite runs in about 10 seconds, and under ASan in about 16.
