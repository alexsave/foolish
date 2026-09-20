# COMMON.md

What werewolf inherited from `foolish` and did not touch, and what it had to
touch that should never have been one game's business.

Two lists, kept as I went. The point of both is a later `core`: the first is what
could move there tomorrow with no argument, the second is what would have been
there already if the tree it came from had been built for two games instead of
one.

The fork point is `689630eb`. Everything below is measured against it
(`git diff --name-only 689630eb HEAD`), not remembered.

---

## Untouched

Inherited and byte-identical. 166 files.

### The two kernel primitives - the strongest core candidates in the tree

| File | Why it survived a change of game |
| --- | --- |
| `c/src/deal_rng.{c,h}` | A crypto-grade seeded CSPRNG with unbiased bounded draws. It shuffles roles here and shuffled cards there; the reasoning in its header (a player legitimately observes SOME outputs of this stream, so a reversible generator lets them run it backwards) is the same reasoning for both, and is MORE load-bearing here - running this backwards from your own role would give you the table. |
| `c/src/sha256.{c,h}` | Freestanding, no libc beyond memcpy/memset. The envelope's parent link and Rule P's digest tiebreak both need a hash that is identical on every device. Nothing about it is a card or a wolf. |

These two are the answer to "what is actually shared". They are 100% of the
inherited C that compiles into this product.

### Tooling that is already game-agnostic

- `tools/structgen/**` (34 files) - generates Swift and TS from C headers, with
  its own test suite. Not wired up here yet (see Wished-for), but nothing in it
  is about either game.
- `tools/datagen/**`, `tools/sgcommon/**`, `tools/llvm.mk`.
- `ios/Tools/rig/lib/*.py` (18 files) - the rig's measurement library: find a
  control by accessibility label in device points, find an edge by colour,
  contact sheets, MSE comparison, tween fitting. `night.sh` (new) uses `ax.py`
  **unmodified**, and exports `FOOLISH_SIM` rather than editing it, precisely so
  the file stays byte-identical for the day these products share a core.
- `ios/Tools/rig/rig.sh`, `ios/Tools/rig/shots/**`, `ios/Tools/IconGen/**`,
  `ios/Tools/store/market.py`, the texture generators.

### Documents that are architecture rather than Durak

- `docs/IMESSAGE_GAME_DESIGN.md` - §7 (Rule P, chains-not-diffs, rebase) is the
  design this product's concurrency is built on. Read it before touching
  `c/src/ww_wire.c`.
- `docs/IMESSAGE_SEAT_IDENTITY_V2.md` - the three §6 signals, ported into
  `c/src/ww_seat.c`.
- `docs/IMESSAGE_LOBBY_V3.md` - the document that explains why creating must
  never deal. I deleted it in the first demolition pass and had to bring it back
  the moment the lobby work started, which is its own argument for a core: it is
  not a Durak document, it is the write-up of a hole every serverless
  correspondence game in a thread can fall into.
- `docs/ARCHITECTURE_AS_A_PATTERN.md`, `docs/SECURITY_WASM_BOUNDARY.md`,
  `docs/PLAYER_VIEWS.md`, `docs/CODEGEN_ALTERNATIVES.md`,
  `docs/IMESSAGE_BODY_CODEC.md`, `docs/IMESSAGE_MAC_RUNBOOK.md`.

### Kept, unread, and honestly stale

- `c/i18n/**` (25 files) - a C string table plus one generated module per
  language. The MECHANISM is exactly right and belongs in a core; the CONTENTS
  are 24 languages of Durak. Nothing here compiles into this product yet.
- `e2e/validation/**` (18 files) - the derived-gate pattern, which is genuinely
  worth keeping. They are stale against this tree (they name deleted paths) and
  there is no npm here to run them with. Kept as the pattern, not as a gate; the
  gate that actually runs is `ios/scripts/release_gate.sh`, which is the same
  idea written for this product.

---

## Wished-for

Things I had to touch, change or rewrite that I believe are generic. One line
each on why, and what the shape should be.

### 1. Seat identity was inside the envelope

`msg_seat_resolve_on_board` / `_in_lobby` / `msg_seat_claimed_by_name` /
`msg_seat_cache_disowned` lived in `msg_wire.c`, among the card game's header
fields. **Nothing in them is about cards**: it is roster arithmetic over
`(seat, name)` rows, plus what Messages will tell an extension about who sent the
bubble it is looking at. I lifted it into its own file (`c/src/ww_seat.c`) and it
came across with only the type name changed.

It is the clearest single example in the tree of a rule that should never have
been game-specific, and it is *more* dangerous here than there: getting a seat
wrong in Durak shows you somebody else's cards; getting it wrong here makes you
somebody else's ROLE.

**Shape for a core:** `core/seat.{c,h}`, over a `{seat, name}` row type the
envelope supplies. Its tests came over too - they were in Swift
(`SeatIdentityTests`) because the logic used to be in Swift, and they are in C
here, where they can be mutation-checked.

### 2. The lobby's rules were inside the envelope too

`msg_lobby_offered`, `msg_lobby_can_exit`, `msg_lobby_can_set_rules`,
`msg_lobby_rules_changed`. Everything except the rules toggle is generic: how
many seats a chat shape holds, who may join, who may start, and the rule that the
newest sender stands aside while there is still room.

**Shape for a core:** `core/lobby.{c,h}` parameterised by `(min_players,
max_players)`, with the game supplying those two numbers and nothing else.
Werewolf's minimum is 5 rather than 2, and that single difference is what makes a
1:1 chat unplayable here - which the lobby has to be able to SAY, not count
toward. A shared lobby needs a `TOO_FEW` verdict the fork never needed.

### 3. The envelope itself is 80% generic

`c/src/ww_wire.{c,h}` is a near-copy of `msg_wire.{c,h}` with a different body.
What is identical: the header shape and offsets, the decode/replay split (hostile
bytes here, semantics there), chains-not-diffs, the zero-copy borrow of the body,
`parent8`, the digest, and **Rule P in full**. What differs: one byte of magic,
the body's record format, and which fields feed clauses 1 and 2.

**Shape for a core:** `core/msg.{c,h}` owning the header, the parse, the bounds
checks, the digest and Rule P, with the game supplying a vtable of three things -
measure a body, replay a body onto a game, seal a game into a body. I did not
build that here because a second copy was the only way to be sure which parts
were really shared; now I am sure, and the list is the whole header.

**Rule P especially.** It is a total preference order that needs no clocks and no
ordering guarantee from the transport, and its hardest clause (a chain's own
direct child beats the parent it names) exists because of a bug that took the
fork weeks to diagnose. Every serverless correspondence game in a message thread
needs exactly this, and none of them should have to rediscover it.

### 4. Per-seat masking is a pattern with no shared code

`c/src/view.c` computed "you only see your own hand" in the kernel rather than in
each client, and `ww_view.c` computes "you only see your own role" the same way.
No line could be shared - the payloads have nothing in common - but the SHAPE
should be: one walker used both to write the blob and to measure it (a separate
measure function is a second implementation of the layout, and they drift), and a
`viewer` argument with reserved negatives for unmasked and spectator.

**Shape for a core:** a tiny `core/put.h` with the `{buf, n}` writer whose sink
may be null. Forty lines, and it is the thing that makes "measure agrees with
put" a mutation somebody can kill (it killed 207 assertions here).

### 5. The Swift-visible bridge should be generated, not written

`c/ios/ios_api.c` is 123-entry-points-of-nine-lines in the fork and ~50 here, all
of the same shape: take ints, call the kernel, return an int. `sdk/swift/` is the
mirror image. `tools/structgen` already generates Swift from C headers and is
sitting right there unused.

**Shape for a core:** point structgen at the bridge header and delete both
hand-written halves. I did not do it in this milestone because the bridge's shape
was still moving, and generating a moving target costs more than it saves - but
`sdk/swift/Kernel.swift` is 317 hand-written lines that a generator should own.

### 6. The extension's lifecycle is the same in every game

`MessagesViewController` - adopt on select, Rule P on receive, stage via insert,
commit on `didStartSending`, drop on cancel, never seal or read across an
`await`. The fork's is 1,197 lines and mine is 326, and the difference is
entirely board rendering: the lifecycle itself is identical and every one of its
comments is a scar from the same platform, not from cards.

**Shape for a core:** a `MessagesSurfaceController` base class owning the
MSConversation, the staged-bubble ledger and the adopt/prefer dance, with the
game supplying a root view and a payload codec.

### 7. The solo seat picker should have been in the box

I was told to port it, and it should not have needed porting: **a correspondence
game in Messages cannot be tested without it.** You cannot add participants to
the real Messages host, so a seven-player night on one simulator is otherwise
impossible - and the night is the whole product. It is the difference between a
suite that passes and a thing anybody has seen work.

Its security half is generic too, and here it is sharper than in the fork:
a release build must never let a player choose which seat they hold, because in
this game choosing a seat is choosing to be the wolf.
`ios/scripts/release_gate.sh` is written for that and would work unchanged for
any game - source half (every mention behind `#if DEBUG || SOLO_TESTING`) plus
binary half (the symbol is not in the Release product), with a note in the file
about which way each half is weak, because a gate whose limits are not written
down gets trusted for things it does not do.

### 8. `DevFlags.flag(key, shipping:)` is the right knob and should be shared

One knob per behaviour, and the SHIPPING value is its argument - so with no flag
file the debug value IS the shipping value and the two builds cannot drift. Forty
lines, and it closes a trap every codebase grows on its own.

### 9. The i18n table mechanism is generic; its contents are not

`c/i18n/` is one C file per language plus a generated module, and that is the
right answer. But the keys and the strings are Durak, so a fork inherits 25 files
of dead weight. **datagen should take the string table as an input**, not bake
one in.

### 10. Small things that cost real time

- **`ios/scripts/mac_tests.sh`** is a per-product script that is 90% the same
  everywhere: build the xcframework, regenerate the project, restore the
  entitlements, run the tests, build the shipping scheme. The `--regen` trap
  (xcodegen only re-reads `project.yml` when it is newer, so a new `.swift` file
  is silently not in the target) is a platform fact, not a game fact.
- **`xcodegen` blanks entitlements** every run. The fork restores them with
  `cp -p` because the mtime matters to Xcode's build description. Every fork of
  this tree will hit it.
- **A simulator build with no `DEVELOPMENT_TEAM` signs ad-hoc and comes out with
  an empty entitlements dict**, so the App Group silently does not exist - and
  the App Group is where the rig's flag file lives, so the rig cannot be switched
  on at all. Cost me a full cycle to find.
- **`ios/vendor/*.xcframework` must be git-ignored.** A stale binary in git is a
  kernel two people disagree about.

---

## What I deliberately did NOT keep

For the record, so a later core reader does not go looking:

- The whole Next.js client, the Supabase functions, and the TS e2e suite that
  tested them. This product has no server: the whole game is the MSMessage URL.
- `c/src/game.c`, `legal.c`, `replay*.c`, every `*_strategy.c`, `cordite_sim.c`,
  `anim_plan.c`, the card board views, and the 60k lines of Swift that drew them.
- `ios/HarnessUI/**` - the fake-transcript harness. The real Messages app plus
  the solo seat picker is what a night actually needs looked at in.
