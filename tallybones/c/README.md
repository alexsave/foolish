# The kernel, its wire and its bridge

```
make -C tallybones/c run          every test: rules and T11, the wire, 1,400 fuzz games,
                                  the words, the beats, two phones through the bridge,
                                  and the bridge smoke
make -C tallybones/c asan         the same under ASan + UBSan (fewer games)
make -C tallybones/c wasm         the kernel as wasm32 objects, freestanding
                                  (WASM_CC=/opt/homebrew/opt/llvm/bin/clang on this Mac)
make -C tallybones/c ios-smoke    every bridge entry point, host compiler, no Mac
make -C tallybones/c structgen    the Swift readers -> tallybones/ios/Generated/TallybonesKernel.swift
make -C tallybones/c datagen      the Swift strings -> tallybones/ios/Generated/i18n/
make -C tallybones/c ios-lib      tallybones/ios/vendor/Tallybones.xcframework (Xcode; runs both above)
make -C tallybones/c swift-smoke  the bridge driven from Swift through the generated readers (a Mac)
./tallybones/c/build/tb_fuzz 20000 3      more games, another stream
./tallybones/c/build/tb_msg_test 200      more games a size through the wire
```

The shape is Pick 'Em Up's (`pickemup/c`), much smaller: `tallybones/docs/DECISIONS.md` is the specification.
The generated Swift and the xcframework are build outputs, never committed.

## The file map

| File | What |
|---|---|
| `src/tb.h`, `src/tb.c` | the rules: scoring, the menu, apply, turns in seat order, leaves, the end, THE one roll derivation, the draft |
| `src/tb_code.h`, `src/tb_code.c` | the body (one mixed-radix digit a bubble) and the one walker: encode, decode, and the resident replay that keeps the body forwards for the rolls |
| `src/tb_internal.h` | the sink and the step `tb.c` shares with the coder and the plan |
| `src/tb_plan.h`, `src/tb_plan.c` | the events of a bubble range, and of a staged move (nothing derived) |
| `src/tb_view.h`, `src/tb_view.c` | the view a host draws: dice, keep marks, every card and total |
| `src/tb_lobby.h`, `src/tb_lobby.c` | Pick 'Em Up's lobby, renamed |
| `src/tb_msg.h`, `src/tb_msg.c` | the envelope: header, roster, check, the race rule, the seat resolver, the peek |
| `src/tb_say.h`, `src/tb_say.c` | which sentence a position says |
| `src/tb_beats.h`, `src/tb_beats.c` | T9's settle, stamp and turn on a clock, the frame at any millisecond, one beat's transform |
| `i18n/keys.h`, `i18n/strings_en.c` | every word, in the shape `shared/tools/datagen` reads |
| `ios/include/tb_api.h`, `module.modulemap` | the one header Swift sees (module `CTallybones`) |
| `ios/tb_api.c` | the bridge: one resident message, one staged move, the send echo, reads, words, beats |
| `ios/tb_lay.c` | the dice tray, pips and scorecard rows |
| `ios/tb_api_layout.h`, `ios/layout.args` | the structs the bridge hands Swift, and what structgen generates for |
| `ios/tb_api_smoke.c`, `.swift` | the bridge surface from C, and from Swift through the generated readers |
| `tests/tb_test.c` | T3 to T6 and the T11 group |
| `tests/tb_msg_test.c` | the wire: round trips, the race, the fit, the sweeps, every bubble of random games |
| `tests/tb_fuzz.c` | random games at 2..8 seats against every invariant, every roll against the spec's hash |
| `tests/tb_say_test.c` | the table's rules, captions word for word, the guards |
| `tests/tb_beats_test.c` | the three motions, and every bubble's plan ending on the settled board |
| `tests/tb_twophone_test.c` | a whole two-player game phone to phone through the bridge |
| `tests/MUTATIONS.md` | the mutation each test was seen to fail on |

## How the T11 rule is held

Every die comes from `tb__roll` in `tb.c`, and only a caller holding a body can make it produce a value.
The bodies come from the walker in `tb_code.c` in its resident modes (decode of a received or sent link, `tb_replay`) and from `tb_new` (the empty history).
`tb_draft`, `tb_plan_move` and the encoder's read-back all step with no body, so a staged KEEP's rerolled dice read 0.
The bridge keeps the staged move beside the resident and never inside it; the reroll exists at `tb_api_mark_sent`, which decodes the sent link, and `tb_api_read` refuses the staged link itself (`TB_ESTAGED`).

## Measured

From `make run` on 2026-09-27: `tb_test` 6,525 assertions, `tb_msg_test` 29,319, `tb_fuzz` 16,424,832, `tb_say_test` 344,326, `tb_beats_test` 459,311, `tb_twophone_test` 457, the C smoke 67 checks, every one 0 failed; `swift-smoke` adds 28 checks from Swift.
Links: the longest link over 20 games a size is 176 characters at 2 seats and 455 at 8; the longest possible game (8 seats, 312 bubbles, 48-byte names) is 1,130, against a compile-time bound of 1,203 and `MSMessage.url`'s 5,000.
Sizes: about 2,800 lines of C in `src/` and `ios/`, 108 KB of wasm32 objects at -Oz, a 136 KB device slice.
