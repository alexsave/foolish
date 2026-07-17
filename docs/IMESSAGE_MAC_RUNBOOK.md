# Mac runbook — two players through iMessage, step by step (2026-07-17)

*The hands-on companion to [`IMESSAGE_SHIP_BLOCKERS.md`](IMESSAGE_SHIP_BLOCKERS.md).
It walks one person with a MacBook through the **B4 live send/accept test** —
two simulated participants playing a full Durak game through iMessage bubbles —
plus the optional credentialed half (real device pair, two Apple IDs) and the
quick wins worth grabbing while a Mac is open.*

**What you need per part:**

| Part | Needs |
| --- | --- |
| 1–3 (build, tests, **two-player simulator game**) | A Mac with Xcode 16+. **No Apple Developer account, no signing, no Apple IDs** — the project ships with signing off and the simulator harness fakes both participants. |
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

1. In Xcode, select the **FoolishMessages** scheme (auto-generated; if the
   scheme list hides it: Product ▸ Scheme ▸ Manage Schemes ▸ check Show).
2. Run on an iPhone simulator. Xcode asks which host app to run in — choose
   **Messages**.
3. Messages opens in the simulator. Open the first seeded conversation, tap
   the **+ / apps** button next to the text field, and find Foolish in the
   app drawer.
   - **Expected blemish:** the tile will be blank/generic — the extension has
     no iMessage icon asset yet (blockers doc B5.1). That's cosmetic here.

### 3.3 Play a full 2-player game

**As player A (conversation 1):**

1. Compact drawer shows "Foolish" + **New game** → tap it. The view expands
   into the table (a fresh 2p game is dealt; you are seat 0, named "Me" —
   the missing nickname editor is B3, expected).
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

### 3.4 The cancel/cache matrix (10 minutes, worth it)

These are the lifecycle edges the unit tests explicitly can't reach:

- **Cancel a staged send:** stage a move, then delete the bubble from the
  input field instead of sending (`didCancelSending`). Re-open the extension:
  it must show the *parent* state, not your abandoned move.
- **Backgrounding mid-stage:** stage a move, switch apps, come back. Staged
  actions live only in memory today — losing them is *known* behavior (B1's
  pending ledger is unbuilt); confirm nothing worse happens (crash, corrupt
  cache).
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
| Everyone is named "Me" | No nickname editor; store default | B3 |
| No way to start a 3+ player game | Genesis hard-codes 2p LIVE; WAITING lobby unbuilt | B2 |
| Concurrent/racing turns don't converge; staged moves lost on incoming bubble | Rule P/rebase primitives exist but aren't wired into the flow | B1 |
| Blank drawer tile | No iMessage icon asset | B5.1 |
| "no battle" / game-over line in English under ru/ko | `MessageBoardView` literals | B6 |

## Part 4 — with credentials: a real device pair (two Apple IDs)

Only this part needs the team. Simulator results usually hold, but §17.12
warns real-device timing differs — do this once before shipping the extension.

1. **Signing setup** (edit `ios/project.yml`, not the xcodeproj):
   - Under `settings.base`: set `DEVELOPMENT_TEAM: <TEAMID>`,
     `CODE_SIGN_STYLE: Automatic`, and delete the
     `CODE_SIGNING_REQUIRED/ALLOWED: NO` lines.
   - `xcodegen generate`, then in Xcode sign in (Settings ▸ Accounts) with a
     team member Apple ID. Automatic signing will create the two bundle ids
     (`cards.foolish.app`, `.MessagesExtension`) and the
     `group.cards.foolish` App Group in the portal on first build.
2. **Devices:** two iPhones, each signed into a *different* Apple ID with
   iMessage active, both on the team (or use TestFlight internal testing
   once the App Store Connect record exists).
3. Build the `Foolish` scheme to each device (the extension embeds
   automatically). On each phone: Messages ▸ conversation with the other
   Apple ID ▸ + drawer ▸ Foolish.
4. Re-run the whole §3.3–3.4 matrix across the pair. Add the real-world
   cases the simulator can't produce: delivery lag (airplane-mode one phone
   mid-turn), lock-screen preview of the bubble (must show only the PUBLIC
   snapshot + summary text), and a reinstall of the app on one phone
   mid-game (seat recovery via §6 — in 2p, sender inference should re-seat
   you without asking).

## Part 5 — while the Mac is open (quick wins from the blockers doc)

Each is small and currently gated only on "someone has a Mac":

1. **Render the app icon** (blockers A4):
   `swift run --package-path ios/Tools/IconGen icongen` → commit the PNG into
   `FoolishApp/Assets.xcassets/AppIcon.appiconset/`. Upload validation fails
   without it.
2. **Record snapshot references** if Part 2 found them missing/drifted.
3. **`make ios-goldens`** if anything engine-side changed; commit if dirty.
4. **Enable macOS CI** (B7): uncomment the `xcode` job in
   `.github/workflows/ios.yml` — simulator build+test needs no signing, only
   macOS runner minutes.
5. With the account: create the App Store Connect record and start the
   `Compliance.md` TODO(F) checklist (demo account, deletion URL, name
   availability).
