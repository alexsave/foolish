# Overnight session handoff (2026-07-18)

Autonomous session while you slept. Everything below is on branch
**`claude/imessage-b4-fixes`** (pushed to origin), building on the B4 punch-list
fixes from earlier. Nothing merged to `main` yet — it's yours to review + merge.

## 1. Move flow simplified — your main ask ✅

> "too many buttons before you can send... as soon as you drag an attack/pickup/
> say good it should take you to the message, so you just hit send after."

**Done.** Playing any move now **auto-stages** the bubble the instant it's
applied — the separate "Send move" button is gone; only **Undo** remains. Your
next action is just the Messages send arrow.

Per-move analysis (all end at "staged", 0 extra send taps):
| Move | Taps | After the last tap |
| --- | --- | --- |
| Attack | tap card | auto-staged |
| Cover | select card → tap battle | auto-staged |
| Pickup ("Take") | one tap | auto-staged |
| Good ("Done") | one tap | auto-staged |
| Pass/transfer | select card → transfer | auto-staged |

Throwing in more cards just re-stages (the input bubble updates). Undo re-stages
the reduced chain. Code: `MessageTableView.stageNow()` after every `apply`.

**Screenshot-verified**: playing a cover lit the send control immediately (see
below).

## 2. FoolishHarness — test 2–8 player games on ONE simulator ✅

Apple's Messages host can't be extended past its 2 seeded participants, and two
sims can't iMessage each other — so a 3–8 player game can't be exercised on the
shipping path (this is what `multisuggestion.txt` correctly diagnosed). So I
built **FoolishHarness**, a dev-only app target (never shipped) that renders the
**same `MessagesRootView`** the extension renders, driven by a fake 2–8
participant transcript with a participant switcher.

Because the game is fully turn-based (state rides the URL; seat identity is
`SeatIdentity`'s pure logic), it's faithful for the group logic + UI. **Each fake
participant gets its own seat cache**, so seat inference resolves automatically —
this is the *real* fix for the single-sim "waiting for Seat 2" (that was a
one-device shared-cache artifact, confirmed: in the harness the defender has
`[cover,pickup,pass]` and plays fine).

**Run it:**
```
xcodebuild build -scheme FoolishHarness -destination 'platform=iOS Simulator,name=iPhone 17' \
  -derivedDataPath ios/build/DD CODE_SIGNING_ALLOWED=NO
xcrun simctl install <udid> ios/build/DD/Build/Products/Debug-iphonesimulator/FoolishHarness.app
xcrun simctl launch <udid> cards.foolish.harness
```
- Top strip: player-count picker (2–8), participant pills (tap to "become" that
  player), the transcript, and a **Send ➤** button (the blue-arrow stand-in —
  lit when a move is staged).
- Play as one participant → hit Send ➤ → tap another participant → they see the
  incoming bubble as the receiver, correctly seated.
- Dev env hooks (screenshotting without taps): `HARNESS_SEED=1` deals a board
  immediately; `HARNESS_AUTOMOVE=1` auto-plays the first legal move. Pass via
  `SIMCTL_CHILD_HARNESS_SEED=1 xcrun simctl launch ...`.

To make this possible, `MessagesRootView` moved `FoolishMessages → FoolishKit`
and its `MSMessagesAppPresentationStyle` became a plain `MsgPresentation` enum
(the extension maps onto it). The shipping extension is unchanged in behavior.

## 3. iMessage icon — zoom-out stopgap committed; outpaint pending your pick

The tight 4:3 crop clipped the Д in the drawer; committed a padded stopgap so it
no longer clips. The **SD-outpainted large master is still cooking** (a
background agent, per your "zoom out even more, big croppable canvas" note) —
when it lands I'll surface candidates. You pick the seed; I wire it into both the
app icon and the iMessage slots.

## What I could NOT do (and why)
- **Drive the sim interactively**: `cliclick` moves the cursor but its synthetic
  clicks don't reach the Simulator (Accessibility permission the headless shell
  lacks). That's why the harness has the env-gated auto-seed/auto-move hooks — so
  I could still screenshot real board states.
- **Snapshot tests**: `ComponentSnapshotTests` render non-deterministically in
  the headless sim (`IOSurfaceClientSetSurfaceNotify failed`), so I couldn't
  re-record `testMessageBoardMidGame` (its colors changed with the red bubble).
  These refs are **not git-tracked** and don't run in Linux CI, so it's
  cosmetic/local — re-record on a real Mac GUI session if you want it green.

## What I validated with the harness (screenshots)
- **Full 2p game to game-over** via `HARNESS_AUTOGAME` (~10 turns): turn handoff,
  seat inference, and auto-stage correct every turn; ended on "… is the fool".
- **4-player board** (`HARNESS_PLAYERS=4 HARNESS_SEED=1`): 4 seat badges with
  correct attacker/defender roles, viewer holds the opening move.
- New-game setup (chat-aware: 2p locks to "Players: 2"), wool table + wood
  buttons, the red-bubble path, and auto-stage lighting the send control.
- One on-device thing to eyeball: the 🃏 in the "… is the fool" line rendered as
  a tofu box in the *headless* sim (same rendering gremlin as the snapshots) —
  almost certainly fine on a real device, but worth a glance.

## Suggested next steps for you
1. Pick an outpainted icon (I'll surface candidates).
2. Run FoolishHarness, play a 3–4 player game across participants, sanity-check
   the lobby + the new auto-stage feel.
3. Run the real Messages 2-participant sim harness (`IMESSAGE_MAC_RUNBOOK.md`
   Part 3) to confirm auto-stage feels right against Apple's actual insert/send.
4. Merge `claude/imessage-b4-fixes` to main when happy.
