# The kernel, its wire and its bridge

```
make -C chuiniu/c run          every test: rules, dice, plan and view, words, 3,000 fuzz games,
                               the wire (cn_msg_test), the three-phone game and the bridge smoke
make -C chuiniu/c asan         the same under ASan + UBSan
make -C chuiniu/c wasm         the kernel as wasm32 objects, freestanding
                               (WASM_CC=/opt/homebrew/opt/llvm/bin/clang on this Mac)
make -C chuiniu/c wasm-roll    the throw (cn_roll.c) and the renderer (cn_scene.c) as a browser module,
                               build/wasm/cn_roll.wasm (SIMD128)
make -C chuiniu/c docs-roll    embed that module in ../docs/UI.html, which plays it
make -C chuiniu/c bench        the renderer's speed: a six-seat frame at 1x to 3x, 1, 2 and 4 bands, and its memory
make -C chuiniu/c ios-smoke    every bridge entry point, host compiler, no Mac
make -C chuiniu/c structgen    the Swift readers   -> chuiniu/ios/Generated/ChuiniuKernel.swift
make -C chuiniu/c datagen      the Swift strings   -> chuiniu/ios/Generated/i18n/
make -C chuiniu/c ios-lib      chuiniu/ios/vendor/Chuiniu.xcframework (Xcode; runs both above)
make -C chuiniu/c swift-smoke  the bridge driven from Swift through the generated readers
./chuiniu/c/build/cn_fuzz 50000 7     more games, another stream
./chuiniu/c/build/cn_msg_test 1000    1,000 games a size through the wire
```

The shape is `pickemup/c`'s, cut to a dice game: one move is one bubble, and there is no draft of several actions, no undo and no layout file.
The kernel is plain C11 with fixed-size structs and no allocation; it reaches `memcpy`, `memset`, `memcmp`, `strlen` and `strncmp` and nothing else, so the same files build for wasm32 and into the xcframework.
`cn_roll.c` is the first three-dimensional thing in a kernel here: a rigid-body throw with its own `sqrt`, `sin` and `cos`, compiled `-ffp-contract=off` (in `WARN`, so every build), and measured bit-identical between gcc, clang and the wasm module over 60 seeds; the host links `-lm` only for the builtins' error path.
`shared/c` (deal_rng, sha256, b32, mixrad) is reached by relative `#include`, as pickemup does, and nothing under `shared/` was changed.

## The file map

| File | What |
|---|---|
| `src/cn.h`, `src/cn.c` | the rules (DECISIONS R1 to R8): state, legality and the menu, apply, the call, replay, the hash |
| `src/cn_dice.c` | the dice, derived from already-sent state (K2, K3) |
| `src/cn_plan.h`, `src/cn_plan.c` | the events of a run of moves, through the one apply path |
| `src/cn_view.h`, `src/cn_view.c` | the masked per-seat view and the menu (K10) |
| `src/cn_lobby.h`, `src/cn_lobby.c` | the lobby's verdicts, pickemup's at 2 to 6 seats (K7) |
| `src/cn_say.h`, `src/cn_say.c` | which sentence a position says (K8, K9, K11) |
| `src/cn_code.h`, `src/cn_code.c` | the body: every move as one mixed-radix number (K4) |
| `src/cn_msg.h`, `src/cn_msg.c` | the envelope: header, roster, check, Rule P, the seat resolver and its records (K4 to K6) |
| `src/cn_beats.h`, `src/cn_beats.c` | the plan on a clock and the board at any millisecond (K12) |
| `src/cn_roll.h`, `src/cn_roll.c` | the throw, baked: the cup roll and the table roll as rigid-body frames (K14) |
| `wasm/cn_roll_web.c` | the throw behind scalar exports for a browser (`make wasm-roll`, `make docs-roll`) |
| `src/cn_scene.h`, `src/cn_scene.c` | the renderer: a rasterizer with a shadow map, four pixels at a time, in the caller's arena (48 MB on iOS), in bands on the host's threads; the study and the iOS library run this one file |
| `wasm/cn_scene_web.c` | the renderer for a browser: its 256 MB static arena and the `scene_*` names the study calls |
| `src/cn_internal.h` | the sink `cn.c` shares with `cn_plan.c` |
| `i18n/keys.h`, `i18n/strings_en.c` | every word, one key list, in the shape `shared/tools/datagen` reads |
| `ios/include/cn_api.h`, `module.modulemap` | the one header Swift sees (module `CChuiniu`), every entry point documented |
| `ios/cn_api.c` | the bridge: the resident message, staging, reading, words, beats, two messages |
| `ios/cn_api_layout.h`, `ios/layout.args` | the structs the bridge hands Swift and what structgen generates for |
| `ios/cn_api_smoke.c`, `ios/cn_api_smoke.swift` | the bridge driven phone to phone, from C and from Swift |
| `tests/cn_check.h` | the test helpers on top of `shared/c/test/check.h` (`CHECK`): seeds, the random and the longest-game players |
| `tests/cn_test.c` | the rules |
| `tests/cn_dice_test.c` | the recipe, the golden, same state same dice, and the cancel exploit through the bridge |
| `tests/cn_plan_test.c` | the plan, masking, the menu, the reveal, the beats |
| `tests/cn_say_test.c` | the words |
| `tests/cn_fuzz.c` | random games to the end at 2 to 6 seats against every invariant |
| `tests/cn_msg_test.c` | round trips, the worst case, the every-byte hostile sweep, tampering, lobby, Rule P, seats |
| `tests/cn_twophone_test.c` | a three-seat game phone to phone through `cn_api.h` only |
| `tests/cn_roll_test.c` | the throw over 300 seeds: the mouth, the settle, the hand's fairness, the table roll, the golden |
| `tests/cn_scene_test.c` | the renderer on the host: a lit quad, its shadow where the light says, the open table clear; the six-seat table's pinned picture (clang, gcc and wasm alike); bands of any number drawing the same bits and the same work; a frame or texture that does not fit, a stray index and out-of-range numbers refused cleanly |
| `tests/cn_scene_frame.h` | a six-seat table built as the study builds it (cup, fillet, crown, inside; rounded dice; textures of its sizes with its measured normal-map statistics), shared by the test and the bench |
| `tests/cn_scene_bench.c` | the renderer's speed (`make bench`), not a test: mean and p95 a frame at each scale and band count, a pass's share, a `sample` loop, a picture |
| `tests/MUTATIONS.md` | the mutation each test was seen to fail on |

## Measured

From `make run` on 2026-09-27.
Test counts: `cn_test` 3,763 assertions, `cn_dice_test` 6,962, `cn_plan_test` 1,737,483, `cn_say_test` 7,152, `cn_fuzz` 15,759,233, `cn_msg_test` 89,178, `cn_twophone_test` 9,909, and the bridge smoke 77 checks, every one 0 failed; `swift-smoke` adds 40 checks from Swift.
The 3,000 fuzz games (600 a size) play about 72 moves and 18 calls a game, the longest 160 moves.
The link, over 40 games a size (every bubble): median 131 characters at two seats and 274 at six, p99 152 and 349, max 370.
The longest game there can be (every rank bid in every round, six 48-byte names) is 471 characters at two seats and 3,058 at six, against a compile-time bound of 4,391 and the 5,000 of `MSMessage.url`.
The hostile sweep turns every byte of eight real bubbles to every other value (284,070 corruptions): all but 2 are refused, and those 2 are whole messages that write back to their own bytes.
`cn_scene_test` (added 2026-10-04, moved to `src/` 2026-10-05): 62 assertions, 0 failed; in the study the module draws a throw frame of six full-size cups on a canvas up to the glass's top (699 by 1,521 at 1.5x) in about 18 ms in Chrome (20 before the renderer drew four pixels at a time). `cn_scene_skip(mask)` leaves passes out, for profiling only.
`cn_scene_bench` (2026-10-05, an M1, -O2): the six-seat frame on a 390 by 718 board takes, on one thread, 9.1 ms at 1x, 14.2 at 1.5x, 21.0 at 2x and 37.7 at 3x (the renderer before this date: 8.9, 15.1, 24.0, 47.3), and in 16 bands 4.8, 6.8 and 12.0 at 1.5x, 2x and 3x; any band count draws the same bits as one thread. A 2x frame takes 33.9 MB of the arena, so with a 48 MB arena the textures must be one shared set (9.6 MB with their copies), not every seat's own (57.7).
`cn_roll_test` (added 2026-10-04): 16,217 assertions, 0 failed (the big cup across shake lengths, and a far seat's small cup at 4 points' margin); a cup roll bakes in about 2 ms natively and as wasm, 200 to 320 frames; over 300 seeds no die corner comes within 12 points of the mouth, none is placed by the last resort, the slowest settle is 2 s, the face up before the turn recurs 17% and its opposite 19%, and the six faces come up evenly (χ² 8 over five degrees at 1,500 dice, 10 at 4,500; the first integrator favoured two faces by a sixth, and this number is what caught it).
`make run` takes about 20 seconds, `make asan` about 12.
