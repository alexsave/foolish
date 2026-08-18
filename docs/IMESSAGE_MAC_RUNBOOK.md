# Mac runbook — two players through iMessage, step by step (2026-07-17)

*The hands-on companion to [`IMESSAGE_SHIP_BLOCKERS.md`](IMESSAGE_SHIP_BLOCKERS.md).
It walks one person with a MacBook through the **B4 live send/accept test** —
two simulated participants playing a full Durak game through iMessage bubbles —
plus the optional credentialed half (real device pair, two Apple IDs) and the
quick wins worth grabbing while a Mac is open.*

**What you need per part:**

| Part | Needs |
| --- | --- |
| 1–3 (build, tests, **two-player simulator game**) | A Mac with Xcode 16+. **No Apple Developer account login needed for the simulator** — Automatic signing is configured project-wide (`8aafbbf`) but simulator builds don't require an active signed-in account, and the simulator harness fakes both participants. |
| 4 (device pair over real iMessage) | Apple Developer team (signing) + two iPhones signed into two different Apple IDs. |
| 5 (quick wins) | Same Mac; some items want the account. |

Read before starting: the extension is **known-incomplete** in specific ways —
§3.6 lists what will *correctly* fail so you don't chase ghosts.

---

## Part 1 — bootstrap (once per machine, ~10 min)

```bash
xcode-select --install                # CLT, if not already present
brew install xcodegen

git clone <repo> foolish && cd foolish
cd c && make ios-lib                  # → ios/vendor/Foolish.xcframework
make ios-smoke ios-view-test          # sanity: both green (same checks as Linux CI)
cd ../ios && xcodegen generate        # → Foolish.xcodeproj (a build artifact)
```

Never hand-edit `Foolish.xcodeproj` or `vendor/` — change `project.yml` /
`c/ios/*` and regenerate (`ios/README.md`).

## Part 2 — build + test baseline (~10 min)

```bash
cd ios
xcodebuild -project Foolish.xcodeproj -scheme Foolish \
  -destination 'platform=iOS Simulator,name=iPhone 16' build test
```

Expected state, per the 2026-07-17 handoff STATUS:

- Everything green **except possibly 4 DesignSystem snapshot tests** — they
  drift with the simulator OS, are environmental, and don't touch iMessage
  code. If they're the only reds, proceed.
- If snapshot references are missing entirely on your machine, record them
  once: set `record = true` in `ComponentSnapshotTests`, run, commit
  `__Snapshots__/`, set it back (`IOS_APP_DESIGN.md` §17.6 step 4).

## Part 3 — the two-player iMessage game (B4), no credentials

### 3.1 How the harness works

The iOS **simulator's Messages app is a built-in two-participant test
harness**: it ships with a seeded conversation pair, and anything you send
from one side arrives in the second conversation as the *other* participant.
You play both seats by switching conversations — no Apple IDs, no network,
no signing. This is exactly the leg nothing has verified yet: everything up
to `conversation.insert` is unit-proven
(`ios/FoolishTests/MessageTurnControllerTests.swift`), and Apple's
insert → send → receive plumbing is what you're about to exercise.

### 3.2 Launch the extension

**Corrected 2026-07-18** — `e9b9120` made the iMessage game its own standalone
App Store product (`cards.foolish.msg`), no longer embedded in the `Foolish`
host-app scheme. The scheme to run is **`FoolishMessagesApp`**, not
`FoolishMessages` (that's the extension *target*, not a runnable scheme) and
not `Foolish` (which no longer embeds the extension at all — see
`ios/project.yml`'s own comments on the `Foolish` target).

1. In Xcode, select the **`FoolishMessagesApp`** scheme (project.yml's own
   comment: *"Run this scheme to launch Messages in the simulator with the
   extension installed"*). If the scheme list hides it: Product ▸ Scheme ▸
   Manage Schemes ▸ check Show.
2. Run on an iPhone simulator. Because `FoolishMessagesApp` is
   `LSApplicationLaunchProhibited` (no Home Screen presence by design — a
   standard Messages-only app), Xcode should launch straight into Messages
   with the extension installed; if it instead asks which host app to run in,
   choose **Messages**.
3. Messages opens in the simulator. Open the first seeded conversation, tap
   the **+ / apps** button next to the text field, and find Foolish in the
   app drawer.
   - The tile should now show the real jester-Д icon (the iMessage app-icon
     gap from the original blockers doc — B5.1 — closed on `main` since this
     runbook was first written; if it's still blank/generic on your build,
     that's a regression worth filing, not expected behavior).

### 3.3 Play a full 2-player game

**As player A (conversation 1):**

1. Compact drawer shows "Foolish" + **New game** → tap it. The view expands
   into the **New game setup** (B3/B2): a name field and a 2-4 player picker.
   Type a name, leave the picker on **2**, tap **Start game** → a fresh 2p
   game is dealt and you are seat 0, named what you typed.
2. Play your opening move(s) with the tap grammar (tap card → action bar).
   Try **Undo** once — the board should rebuild to before the last staged
   action.
3. Tap **Send move**. The bubble (board snapshot + "Your move" caption)
   lands in the Messages **input field** — this is `insert`, a *stage*.
4. Press the blue **send arrow** yourself. (Design rule: only the human
   sends. This also fires `didStartSending`, the only place the seat cache
   commits.)

**As player B (conversation 2):**

5. Go back to the conversation list and open the **second** conversation —
   the bubble you just sent is there, incoming.
6. Tap the bubble → the extension expands, decodes, and **adopts** the chain.
   Verify:
   - You're seated as the *other* seat automatically (2p sender-inference:
     you didn't send the bubble, so you're the non-sender seat — §6.2).
   - **Your hand is face-up; A's hand is hidden.** The bubble *image* itself
     must show no hands (it's the PUBLIC snapshot — check the thread).
7. Play a legal reply, Send move, blue arrow.

**Alternate turns** (switch conversations each time) until game over.

**At game end:**

8. Whoever applies the terminal action stages the **FINISHED** bubble.
   Verify: the summary line names the fool ("… is the fool"), and tapping
   the bubble opens `https://foolish.cards/<code>` — the web **replay** page,
   not `/m/` (the §12 funnel; the kernel emits v6 when re-derivable, else v5).
   Paste the code into the web replay page to double-check it decodes.

### 3.3b The N≥3 WAITING lobby (B2/B3)

The two-participant sim harness can't host 3 real people, but you can still
verify the lobby state machine end to end across the two conversations:

1. As player A, **New game → pick 3 players → Start**. Instead of a board you
   get the **lobby**: seat 1 = your name, seats 2-3 "Open seat", and a **Send
   invite** button. Tap it, send the bubble (its summary reads "tap to join").
2. As player B (conversation 2), tap the bubble → the **lobby** opens with a
   name field and **Join as …**. Type a name, tap Join → you claim seat 2, a
   reseal WAITING bubble stages; send it.
3. Back as A, tap the newest bubble: the lobby now shows two names + one open
   seat. Claim the last seat yourself (or bounce back to B) → the claim that
   **fills the last seat** seals a LIVE handoff ("game on"), sends, and the
   next open drops into the board with the kernel's first attacker up.
4. From there it plays exactly like the 2p game above.

### 3.4 The cancel/cache matrix (10 minutes, worth it)

These are the lifecycle edges the unit tests explicitly can't reach:

- **Cancel a staged send:** stage a move, then delete the bubble from the
  input field instead of sending (`didCancelSending`). Re-open the extension:
  it must show the *parent* state, not your abandoned move.
- **Backgrounding mid-stage (B1 durable ledger):** stage a move, switch apps,
  come back. The move is mirrored into the App Group pending ledger, so it
  should survive; at minimum nothing worse happens (crash, corrupt cache).
- **Rule P stale bubble (B1):** after several turns, scroll up and tap an
  OLDER collapsed bubble for the same game. You must get the "This game has
  moved on" banner (Open the latest / View this anyway), NOT a silent revert
  to the old state.
- **One bubble per game:** after a few turns, confirm the thread shows the
  latest interactive bubble plus collapsed summary-text lines for older
  turns (MSSession + non-nil summaryText). If old turns vanish entirely,
  summaryText broke.
- **The `/m/` fallback:** long-press a LIVE bubble → copy its URL → open in
  Safari (or any browser). The `/m/` page must render the read-only
  spectator board with install/play CTAs.

### 3.5 Record what you find

File anything broken as an issue tagged `imessage-b4`. If it all passes,
update the STATUS block in `IMESSAGE_IMPLEMENTATION_HANDOFF.md` — B4 is the
last unverified M3 leg.

### 3.6 Known-incomplete — do NOT file these as new bugs

From the blockers doc (`IMESSAGE_SHIP_BLOCKERS.md` §4):

| You'll see | Why | Tracked as |
| --- | --- | --- |
| Blank drawer tile | No iMessage icon asset | B5.1 |
| "no battle" / game-over line in English under ru/ko | `MessageBoardView` literals | B6 |

**B1-B3 are now built** (branch `claude/chain-b-imessage`) - they are things to
TEST on this Mac session, not known gaps: the New game setup + nickname field
(B3), the 3-4 player WAITING lobby with join flow (B2), and Rule P adoption /
Rule R rebase / the durable pending ledger (B1). Drive them per §3.3-3.4. Note
they build against a fresh `make ios-lib` (new C symbols): run Part 2 first.

## Part 4 — with credentials: a real device pair (two Apple IDs)

Only this part needs the team. Simulator results usually hold, but §17.12
warns real-device timing differs — do this once before shipping the extension.

1. **Signing** is already configured (`DEVELOPMENT_TEAM: 8N2Z544SB4`,
   `CODE_SIGN_STYLE: Automatic` in `ios/project.yml`, since `8aafbbf`) —
   nothing to edit. Just sign in to Xcode (Settings ▸ Accounts) with a team
   member Apple ID. Automatic signing should create the standalone app's
   bundle ids (`cards.foolish.msg`, `.MessagesExtension`) and the
   `group.cards.foolish.msg` App Group in the portal on first device build —
   confirm they match what's registered (see the submission doc §1a for the
   one still-open icon question).
2. **Devices:** two iPhones, each signed into a *different* Apple ID with
   iMessage active, both on the team (or use TestFlight internal testing
   once the App Store Connect record exists).
3. Build the **`FoolishMessagesApp`** scheme to each device (this is the
   standalone container — see the 2026-07-18 correction in §3.2 above; the
   `Foolish` host-app scheme no longer embeds the extension at all). On each
   phone: Messages ▸ conversation with the other Apple ID ▸ + drawer ▸
   Foolish.
4. Re-run the whole §3.3–3.4 matrix across the pair. Add the real-world
   cases the simulator can't produce: delivery lag (airplane-mode one phone
   mid-turn), lock-screen preview of the bubble (must show only the PUBLIC
   snapshot + summary text), and a reinstall of the app on one phone
   mid-game (seat recovery via §6 — in 2p, sender inference should re-seat
   you without asking).

## Part 5 — while the Mac is open (quick wins from the blockers doc)

Each is small and currently gated only on "someone has a Mac":

1. ~~Render the app icon~~ — **done**, both icons (host app + the iMessage
   4:3 `.stickersiconset`) are committed jester-Д art, Mac-build-verified
   (`351820e`, `51688c5`). Nothing to do here.
2. **Record snapshot references** if Part 2 found them missing/drifted.
3. **`make ios-goldens`** if anything engine-side changed; commit if dirty.
4. **Enable macOS CI** (B7): uncomment the `xcode` job in
   `.github/workflows/ios.yml` — simulator build+test needs no signing, only
   macOS runner minutes.
5. With the account: create the App Store Connect record for **`cards.foolish.msg`**
   (not `cards.foolish.app` — the iMessage game is its own standalone record
   now, `e9b9120`) and paste in `docs/IMESSAGE_APP_STORE_SUBMISSION.md`,
   which has every field drafted (App Privacy, age rating, review notes, a
   verified demo replay code).
