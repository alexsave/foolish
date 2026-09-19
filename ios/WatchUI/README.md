# WatchUI - parked watchOS client

**This code does not compile. It is kept on purpose.**

It is the finished screen design for Foolish on the wrist - Option H, "all
vertical" - written against the July 2026 kernel and landed as source in
`bf351671` ahead of the engine that would run it.
The engine never arrived, and the kernel moved a long way underneath it since.
Nothing depends on this folder, nothing in CI compiles it, and the iOS and
iMessage apps are unaffected by it.

The design itself is not stale.
It was reviewed against a running simulator five times and every number on the
face is a measured one.
Read `docs/WATCHOS_LAYOUT.md` §4.6 and §4.6.1 before changing a single constant
here: §4.6.1 is the as-built record of nine owner calls, several of which were
arrived at by measuring luminance on the simulator, and the original mocks did
not show them. Those mocks were `docs/watchos-layout.html` (removed 2026-09-18, now that the design is locked; recoverable at `570d734c`).

## What the screen is

One game screen plus a roster page.

- **Top**: a two-row InfoLine - labels `FLIP|TRUMP · DECK · DISCARD` over
  *card icon · number · number*.
  Under it the **SeatStrip**, one hand count per player, coloured by state only
  (red = still owes the opening attack, orange = defender, green = said GOOD,
  dark gray = escaped).
- **Left**: the **table as a vertical list**, one bout per row, cover ▸ attack.
  Centred in its column until it overflows, then it scrolls under a drag, with
  edge fades.
  This is the only drag in the app.
- **Right**: the **hand as a crown-driven fisheye lane** hugging the crown edge.
  The Digital Crown moves through your legal options - legal cards, then the one
  terminal action (✓ GOOD, or a red ↓ PICKUP for the defender).
  The verb is a gray 7 pt ALL-CAPS caption directly under the focused card.
  Tap the focused card to commit.
- **One page right**: the **roster** - names, counts, roles, the flip.
  Reached by a right-to-left swipe or by tapping the seat strip.
  Opening on the table is load-bearing: it keeps watchOS's left-edge back-swipe
  unshadowed.

`HTuning.swift` holds every layout constant; no view holds a literal.
`Previews.swift` renders the real screens on static fixtures, with no kernel and
no deal, which is the whole tuning loop.

## Why it does not build

`WatchGame.swift`, `Previews.swift`, `Overlays.swift` and `HTuning.swift` are
written against types that the watch target cannot see, and in some cases
against types that no longer exist in that shape:

| What it wants | Where it is now |
|---|---|
| `Card`, `Move`, `GameView`, `BattleView`, `PlayerView` | generated into `sdk/swift/gen/` by `make -C c ios-lib`; that directory is no longer committed |
| `LocalGame` (the offline bot session) | `sdk/swift/bots/LocalGame.swift`, target `FoolishBots` |
| `LocalGame.botPacingMS` | **gone** - the per-surface pacing knob §4.6.4 describes was never ported out of `claude/ios-redesign` |
| `LocalGame.init(preview:legal:)` | **gone** - today's initialiser is `init(players:botStrategy:)` |
| a watchOS slice of the engine | does not exist; `c/Makefile`'s `ios-lib` builds iOS device + simulator only |

The `WatchFoolish` target in `ios/project.yml` therefore compiles these sources
with **no dependencies at all**, which is why `xcodebuild -scheme WatchFoolish`
fails and why nothing else does.
That target is a deliberate reservation of the registered bundle id
`cards.foolish.app.watchkitapp`, not an oversight.

## What reviving it costs

In rough order, and none of it is watch-UI work:

1. Add watchOS slices to `c/Makefile`'s `ios-lib` (arm64 + arm64_32 watchos,
   arm64/x86_64 watchsimulator) so `Foolish.xcframework` and
   `FoolishBots.xcframework` carry a watch platform.
2. Give the Swift modules the watch needs a watchOS destination.
   `FoolishKit` as a whole will not go: it is full of UIKit, Messages and
   phone-only board code.
   The watch needs the generated models plus `FoolishBots`, so the likely answer
   is a small watch-safe module rather than a destination on `FoolishKit`.
3. Reconcile the four API drifts in the table above.
4. Only then add `dependencies:` to the `WatchFoolish` target in
   `ios/project.yml`.

Checked and **not** a problem, so nobody spends time on it twice: identity moved
into a Roster blob server-side, but `PlayerView.name` is still there in
`sdk/swift/Models.swift`, and `WatchGame.name(for:)` already generates a stable
display name when a seat's kernel name is empty - which is every offline bot
seat. The roster screen needs nothing.

## Where it came from

Two branches, and they are not two designs:

- `savepoint/watch-pre-optionG-f279baa` is a **strict ancestor** of
  `claude/ios-redesign`.
  Its tip is Option G, the layout before the owner's final review.
- `claude/ios-redesign` continues past it through Option G and then Option H,
  which is this code.

Landing H means landing the savepoint too; there is nothing in the savepoint
that H does not already contain.
The eleven Swift files here are byte-identical to `claude/ios-redesign`'s tip.
That branch also carries a phone-app redesign and an online-lobby effort that
have long since landed on main by other routes, so it is not worth rebasing as a
whole - these files and the design docs are all that is still unique to it.
