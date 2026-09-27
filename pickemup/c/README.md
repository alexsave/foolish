# The kernel, its wire and its bridge

```
make -C pickemup/c run          every test: rules, masking, plan, words, 2,800 fuzz games,
                                the wire (pk_msg_test), the two-phone game, the timeline
                                (pk_beats_test) and the bridge smoke
make -C pickemup/c asan         the same under ASan + UBSan
make -C pickemup/c wasm         the kernel as wasm32 objects, freestanding (needs a wasm clang)
make -C pickemup/c ios-smoke    every bridge entry point, host compiler, no Mac
make -C pickemup/c cross WASM_CC=/opt/homebrew/opt/llvm/bin/clang
                                7.3.7: 100 games natively and in wasm32 under node, compared
make -C pickemup/c structgen    the Swift readers   -> pickemup/ios/Generated/PickemupKernel.swift
make -C pickemup/c datagen      the Swift strings   -> pickemup/ios/Generated/i18n/
make -C pickemup/c ios-lib      pickemup/ios/vendor/Pickemup.xcframework (Xcode; runs both above)
make -C pickemup/c swift-smoke  the bridge driven from Swift through the generated readers (a Mac)
make -C pickemup/c beats-dump   the timeline of two takes as Markdown tables (docs/MOTION_REPORT.md)
make -C pickemup/c arena        the bots against each other, 2,000 games a line-up at 2, 4 and 8 players
./pickemup/c/build/pk_fuzz 50000 7        more games, another stream
./pickemup/c/build/pk_msg_test 10000      7.4.1's 10,000 games a size through the wire
```

CI: `.github/workflows/pickemup.yml` runs `run`, `asan`, `structgen` and `datagen` on Linux (D49).

`pk.c` is the rules of `pickemup/docs/RULES_AND_KERNEL.md` and nothing else: the deal, legality, apply, seal and undo.
Every rule a host could ask about is answered here, so a host never re-decides one.
The struct is fixed-size plain integers with no pointers and no bitfields, there is no allocation, and the only libc it reaches is `memcpy`, `memset`, `memcmp`, `strlen` and `strncmp`, so the same files build for wasm32 and into an xcframework.

## The file map

| File | What |
|---|---|
| `src/pk.h`, `src/pk.c` | the rules: cards, state, deal, legality, apply, seal, undo by replay, the hash |
| `src/pk_deck.c` | card ids 0..103 and the one Fisher-Yates, keyed from `shared/c/deal_rng` |
| `src/pk_plan.h`, `src/pk_plan.c` | the animation plan: events per bubble, the draft's own, "since last seen" |
| `src/pk_view.h`, `src/pk_view.c` | the masked per-seat view |
| `src/pk_lobby.h`, `src/pk_lobby.c` | the lobby's verdicts, join, leave, start |
| `src/pk_say.h`, `src/pk_say.c` | which sentence a position says, composed by key |
| `src/pk_code.h`, `src/pk_code.c` | the body: every choice since the deal as one mixed-radix number (`shared/c/mixrad`) |
| `src/pk_msg.h`, `src/pk_msg.c` | the envelope: header, roster, check, lobby rows, Rule P, the seat resolver and its records |
| `src/pk_beats.h`, `src/pk_beats.c` | the motion timeline: a plan's events laid out as beats on `UI.html`'s clock, the frame at any millisecond, one beat's transform (ANIMATION_DECISIONS A1) |
| `src/pk_internal.h` | the sink `pk.c` shares with `pk_plan.c` and the tests |
| `i18n/keys.h`, `i18n/strings_en.c` | every word, one key list, in the shape `shared/tools/datagen` reads |
| `tests/pk_check.h` | the test helpers on top of `shared/c/test/check.h` (`CHECK`): hand-built tables |
| `tests/pk_bot.h` | the random tests' bot, its randomness and the wide seeds, freestanding |
| `tests/pk_cross.c`, `tests/pk_cross.mjs` | 7.3.7: the same games natively and in wasm32 (node), compared by `make cross` |
| `tests/pk_test.c` | legality (7.1), effects (7.2), the deck (7.3), call-out windows (7.7), lobby and edges (7.8) |
| `tests/pk_rules_test.c` | the edges a conformance review found untested: one-card timing and collisions, penalties that run the deck dry, the stops, the history cap, undo and the reshuffle, the resolver after a leave |
| `tests/pk_plan_test.c` | masking (7.5) and the plan (7.6) |
| `tests/pk_say_test.c` | the words (6) |
| `tests/pk_fuzz.c` | random play at 2..8 players against every invariant |
| `tests/pk_twophone_test.c` | the `SIM_VERIFICATION.md` game played phone to phone through the bridge, every bubble checked against the test's own events, counts and captions |
| `tests/pk_msg_test.c` | the wire (7.4), Rule P (7.7.4), seat resolve (7.8.7), the tamper, corruption and truncation sweeps |
| `tests/pk_beats_test.c` | every row of the motion grid against `UI.html`'s demo numbers, the A/B/C channels, and the arrival budgets over 400 games a size |
| `tests/pk_beats_dump.c` | `make beats-dump`: the two takes of `docs/MOTION_REPORT.md` |
| `ios/include/pk_api.h`, `module.modulemap` | the Swift-visible face (module `CPickemup`), the only header the xcframework carries |
| `ios/pk_api.c` | the bridge: one resident message, lobby, staging, reading, words, the beats of a plan, two messages |
| `ios/pk_lay.c` | the layout numbers of `UI.html` (hand row, seat ring, fan, deck, pile, pill slots), so Swift derives none |
| `ios/pk_api_layout.h` | the structs the bridge hands Swift (`PkApiTable`, `PkApiEvents`) |
| `ios/layout.args` | what structgen generates Swift for: those, `PkView`, `PkSince`, and the constants |
| `ios/pk_api_smoke.c`, `pk_api_smoke.swift` | the bridge driven phone to phone, from C and from Swift |
| `src/pk_belief.h`, `src/pk_belief.c` | the bot's belief: what one seat can deduce from the public events, and a sampler of consistent worlds |
| `src/pk_bot.h`, `src/pk_bot.c` | the bots: random, greedy and Monte Carlo (octogen's shape), the knobs, and the table driver |
| `tests/pk_bot_test.c` | the bots: legality everywhere, the belief against the truth, hand-built histories, determinism, the wild's suit, MC against random |
| `tools/pk_arena.c` | `make arena`: line-ups of the bots, both seat orders, a 95% interval on each side's win share |
| `tests/MUTATIONS.md` | the mutation each test was seen to fail on |

The envelope reaches the game only through `pk__new`, `pk_legal_turn`, `pk_is_legal`, `pk_apply`, `pk_seal` and the read-only fields; its lobby verdicts are `pk_lobby.h`'s.

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
`PkView` has no slot for another seat's count while the game is played: every other seat's fan is drawn at `PK_FAN_BACKS` backs, and `reveal` is filled only once the game is over.

**The words are keys.**
`pk_say.c` never formats a user string: every sentence is a key filled by `pk_fill`, numbers go through `pk_itoa`, and a caption is built from the bubble's own events (`pk_say_caption_of`), so a host holding a plan can caption it without a second replay.

**The body is the rules' menus.**
`pk_code` stores, for each decision, which option of the kernel's own menu was taken: the sender, "Last card!", the catch and its target, whether a turn follows, each turn action as an index into `pk_legal_turn`, and whether a two-player turn that came back goes on (D7).
Every digit is a menu of the options that make a real bubble (D40), and one walker runs both directions, so the encoder and the decoder cannot disagree about a menu and every body that decodes re-encodes to its own bytes.

**The envelope reads back what it writes.**
`pk_msg_encode` derives the phase, `bubbles`, `turns` and TIP_SAID from the game and then decodes its own output before handing it over, so no host can emit a payload this build would refuse.
Decode never reads past `in[n-1]`: every field is bounds-checked before it is read, the two-byte check turns a cut link into a refusal, and the header must agree with the replay.
The tests prove it under ASan by decoding every prefix and every corrupted byte of a pool of envelopes placed at the very end of a static array.

**Seats are witnessed, not asserted.**
Which seat is mine is the record (the tag this device sat with, keyed by the game, so a leave that moves rows cannot fool it, D42), then the tag, then the sender fact, then the nickname, with foolish's lobby gate (D47).

**Swift never learns a byte layout.**
`pk_api.h` returns `const void *` into the kernel's storage, and Swift reads it through `read*` functions structgen writes from the kernel's headers; the library is stamped with that layout's hash (`pk_api_layout_hash`) and the module carries it as `SG_LAYOUT_HASH` (D46).
Both are build outputs, never committed.
The resident message is one slot: `pk_api_read` adopts, nothing seals or reads across an await, and `pk_api_text` seals a copy of the draft so the staged bubble can still be undone.

## The bots

An offline capability for simulation and evaluation, not in the app (DECISION D63): `BOT_SRC` is linked only into `pk_bot_test` and `pk_arena`, never into `SRC`, the wasm objects or the xcframework.
The reference is foolish's octogen, and `docs/RULES_AND_KERNEL.md` section "Bot" says piece by piece what was imitated and what was not.

- `PK_BOT_RANDOM` is uniform over the whole turn menu (D64), the baseline.
- `PK_BOT_GREEDY` plays if anything plays (staying in the suit held most, shedding action cards, hitting a seat on two cards or fewer, keeping wilds), else draws once, then passes; it is also MC's rollout policy.
- `PK_BOT_MC` builds a belief from the public events (`pk_belief.h`: exact hand sizes, buried cards, a hard void from a bare pass, soft voids from draws), samples worlds from it, rolls every candidate out with greedy in the same worlds, and keeps the best through three stages; the knobs are `PkBotKnobs`.
- `pk_bot_choose` returns one entry of the kernel's own menu (or a seal it allows), so a bot cannot make an illegal move; "Last card!" and "Caught you!" are one shared rule (D60).
- `pk_bot_round` is the table driver the arena and the tests use (D61).

```
make -C pickemup/c arena                       ARENA_GAMES=2000 ARENA_JOBS=8 by default
./pickemup/c/build/pk_arena 400 8 24 x          400 games, 2 and 4 players, every line-up
PK_DEPTH=0 PK_W1=96 ./pickemup/c/build/pk_arena 400 8 2 1    a knob override, MC vs greedy only
```

Measured on 2026-09-27, 2,000 games a line-up (`docs/BOT_REPORT.md` has every row):

| Players | MC vs random | MC vs greedy | MC vs MC with a random wild suit |
|---|---|---|---|
| 2 | 98.4% [97.8, 98.9] | 55.5% [53.3, 57.7] | 55.5% [53.4, 57.7] |
| 4 | 97.9% [97.3, 98.5] | 54.5% [52.4, 56.7] | 49.4% [47.2, 51.5] |
| 8 | 97.1% [96.4, 97.8] | 51.7% [49.5, 53.9] | 50.4% [48.3, 52.6] |

## Measured

From `make run` on 2026-09-27.
Test counts (the open-items pass, 2026-09-27): `pk_test` 11,089 assertions, `pk_rules_test` 199, `pk_plan_test` 1,193,089, `pk_say_test` 55,609, `pk_fuzz` 3,921,125, `pk_msg_test` 298,141, `pk_twophone_test` 2,420, `pk_beats_test` 233, `pk_arrange_test` 49,995, `pk_bot_test` 107,612 (28,577 at the ASan scale), and the bridge smoke 840 checks (its played-out game is shorter since it carries a wrong call, I40), every one 0 failed; `swift-smoke` adds 25 checks from Swift, and `make cross` compares 100 games natively and in wasm32.
Over the 2,800 fuzz games (400 at each table size): about 188 turn actions and 136 bubbles a game, the longest 1,079 actions, 3,108 reshuffles in all, and three games ended by the long-game stop (D23).
The wire, over `make run`'s 30 games a size (every bubble of every game), in link characters per bubble:

| Players | median | p95 | p99 | p99.9 | max | bubbles |
|---|---|---|---|---|---|---|
| 2 | 194 | 386 | 455 | 472 | 474 | 4,092 |
| 3 | 187 | 283 | 306 | 314 | 317 | 3,112 |
| 4 | 243 | 471 | 549 | 576 | 579 | 5,008 |
| 5 | 235 | 351 | 402 | 426 | 429 | 3,463 |
| 6 | 263 | 389 | 439 | 464 | 467 | 3,661 |
| 7 | 295 | 458 | 573 | 607 | 610 | 4,411 |
| 8 | 320 | 699 | 853 | 890 | 893 | 4,389 |

The owner's p99 case (8 players, 40 turns, six draws a turn, ten catches) measures 341 characters at the median and 347 at p99 over 1,000 deals, against 4.5's estimate of 530.
The 1,500-action stop at two players is 1,883 characters, and the longest eight-player game with eight 48-byte names (750 bubbles) is 1,928; the analytic bound on any game, asserted at compile time, is 4,720 against the 5,000 of `MSMessage.url`.
The whole suite runs in about a minute, and under ASan in about 30 seconds.
