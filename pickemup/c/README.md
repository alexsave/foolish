# The kernel, its wire and its bridge

```
make -C pickemup/c run          every test: rules, masking, plan, words, 2,800 fuzz games,
                                the wire (pk_msg_test) and the bridge smoke
make -C pickemup/c asan         the same under ASan + UBSan
make -C pickemup/c wasm         the kernel as wasm32 objects, freestanding (needs a wasm clang)
make -C pickemup/c ios-smoke    every bridge entry point, host compiler, no Mac
make -C pickemup/c structgen    the Swift readers   -> pickemup/ios/Generated/PickemupKernel.swift
make -C pickemup/c datagen      the Swift strings   -> pickemup/ios/Generated/i18n/
make -C pickemup/c ios-lib      pickemup/ios/vendor/Pickemup.xcframework (Xcode; runs both above)
make -C pickemup/c swift-smoke  the bridge driven from Swift through the generated readers (a Mac)
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
| `src/pk_internal.h` | the sink `pk.c` shares with `pk_plan.c` and the tests |
| `i18n/keys.h`, `i18n/strings_en.c` | every word, one key list, in the shape `shared/tools/datagen` reads |
| `tests/pk_check.h` | the harness: `CHECK`, hand-built tables, the random bot |
| `tests/pk_test.c` | legality (7.1), effects (7.2), the deck (7.3), call-out windows (7.7), lobby and edges (7.8) |
| `tests/pk_plan_test.c` | masking (7.5) and the plan (7.6) |
| `tests/pk_say_test.c` | the words (6) |
| `tests/pk_fuzz.c` | random play at 2..8 players against every invariant |
| `tests/pk_msg_test.c` | the wire (7.4), Rule P (7.7.4), seat resolve (7.8.7), the tamper, corruption and truncation sweeps |
| `ios/include/pk_api.h`, `module.modulemap` | the Swift-visible face (module `CPickemup`), the only header the xcframework carries |
| `ios/pk_api.c` | the bridge: one resident message, lobby, staging, reading, words, two messages |
| `ios/pk_lay.c` | the layout numbers of `UI.html` (hand row, seat ring, fan, deck, pile, pill slots), so Swift derives none |
| `ios/pk_api_layout.h` | the structs the bridge hands Swift (`PkApiTable`, `PkApiEvents`) |
| `ios/layout.args` | what structgen generates Swift for: those, `PkView`, `PkSince`, and the constants |
| `ios/pk_api_smoke.c`, `pk_api_smoke.swift` | the bridge driven phone to phone, from C and from Swift |
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

## Measured

Over the 2,800 fuzz games of `make run` (400 at each table size): about 184 turn actions and 134 bubbles a game, the longest 1,051 actions, about 3,000 reshuffles in all, and two games stopped by the 750-bubble cap.
The wire, over `make run`'s 30 games a size (every bubble of every game), in link characters per bubble:

| Players | median | p95 | p99 | p99.9 | max | bubbles |
|---|---|---|---|---|---|---|
| 2 | 171 | 335 | 376 | 399 | 400 | 2,820 |
| 3 | 200 | 386 | 424 | 447 | 448 | 3,284 |
| 4 | 211 | 408 | 447 | 464 | 466 | 3,186 |
| 5 | 248 | 362 | 389 | 400 | 403 | 4,135 |
| 6 | 282 | 539 | 607 | 634 | 637 | 4,671 |
| 7 | 317 | 611 | 799 | 835 | 840 | 5,518 |
| 8 | 331 | 594 | 781 | 824 | 827 | 5,325 |

The owner's p99 case (8 players, 40 turns, six draws a turn, ten catches) measures 341 characters at the median and 349 at p99 over 1,000 deals, against 4.5's estimate of 530.
The 1,500-action stop at two players is 1,883 characters, and the longest eight-player game with eight 48-byte names (750 bubbles) is 1,928; the analytic bound on any game, asserted at compile time, is 4,720 against the 5,000 of `MSMessage.url`.
The whole suite runs in about a minute, and under ASan in about 30 seconds.
