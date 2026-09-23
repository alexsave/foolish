# UTTT to TestFlight - audit and work plan

Audited 2026-09-22 on branch `claude/uttt-testflight` in the worktree `/Users/alex/Dev/foolish-uttt`.
The spec is `uttt/docs/UI.html` (done, locked).
The pattern is `docs/ARCHITECTURE_AS_A_PATTERN.md`: rules, shape, wire, seat identity and lobby in C; Swift renders and talks to Messages.

## 1. Current true state

### What is green

- `make -C uttt/c run asan ios-smoke` passes (exit 0, no warnings, "bridge ok").
- `make -C uttt/c ios-lib`, `xcodegen generate` and a Debug simulator build of `UtttMessagesApp` succeed.
- A Release device build (`-configuration Release -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO`) succeeds, and `strings` on the Release binaries finds no `dev.seat`, `dev.picker`, "Who are you" or `group.cards.uttt.msg`.
  So nothing DEBUG-only leaks into Release today; `UtttDev.swift` and `UtttSeatChoice.swift` are whole-file `#if DEBUG`.
- Icons: `make -C uttt/c icons` (`uttt/c/tools/icons.sh`) draws every size from the kernel.
  The container `AppIcon.appiconset` has the 1024x1024, and the extension `iMessage App Icon.stickersiconset` has all 11 files (54x40, 64x48, 81x60, 96x72, 120x90, 134x100, 148x110, 180x135, 1024x768, sq-58, sq-87).
  Every PNG was checked with `sips`: correct pixel size, `hasAlpha: no`.
- The built container Info.plist gets `LSApplicationLaunchProhibited = true` automatically from the `application.messages` target type, so that foolish blocker does not apply.
- Played end to end in the real Messages app on a simulator: invitation staged on open, sent, a seeded game opened from both chairs, a move staged, sent, opened by the other seat, answered, re-tapped to change the staged move, and cancelled with the draft's X (the board reverted correctly).
  MSSession folding works: an older move collapses to the caption line "Sent to the top-left board."

### Landed on this branch by a concurrent agent during the audit

- `abee16c3` - Release signs with no App Group (`DebugAppGroup.entitlements` is Debug-only, via `CODE_SIGN_ENTITLEMENTS` in a `configs:` block), `ITSAppUsesNonExemptEncryption = false` is now in the CONTAINER Info.plist, and `MARKETING_VERSION` is 1.0.
- `39284b97` - `uttt/ios/Tools/ship.sh`: archive, manual-signed export with API-fetched store profiles, altool upload, poll to VALID, build number defaults to ASC max + 1.
- Conclusion on the App Group question: nothing in Release needs `group.cards.uttt.msg`.
  The only reader is `UtttDev` (DEBUG), and the Release binary has no reference to the group string.

### What is wrong or missing, found by running it

Screenshots are in `/private/tmp/claude-501/-Users-alex-Dev-foolish/ce7c4549-2c3c-4e25-aeb1-0540c12ff110/scratchpad/shots/` (01 through 19).

1. **BLOCKER - the join never gets staged when the joiner is O.**
   Seat b tapped the invitation, got the board as O ("Waiting on X"), and the compose field stayed empty (`08_join_b.png`, `09_join_collapsed.png`, `10_retap_b.png`, twice).
   `MessagesViewController.join()` calls `stage(sealed, andShowIt: false)` with no conversation, and `stage()` swallows the insert result (`conversation.insert(message) { _ in }`).
   With no sealed bubble the creator never learns the roster sealed, and the game is dead after the invitation.
   Hypothesis: the tap opens the drawer EXPANDED and the insert is refused or dropped mid-transition; the error is discarded so nothing says why.
2. **Spec divergence - who goes first.**
   UI.html "Lobby and end" says "The joiner goes first" (so the joiner is X, and screen 03 reads "First move is yours").
   The code (`UtttWire.mark(of:)`) derives X from SHA-256 of both seat tags, so the joiner is O half the time, which is exactly what creates the empty "Both seats taken." bubble the spec never has.
   Following the spec also removes blocker 1 structurally: the join always carries the first move, so there is never a claim-only bubble.
3. **The wire and the seat rules are Swift.**
   `uttt/ios/UtttKit/UtttWire.swift` hand-builds a query URL (`?v=1&s=&a=&b=&g=`), does base64url, hashes seat tags with CryptoKit, and decides X/O - all rules on the wrong side of the line.
   `UtttModel.tap(at:)` computes block and cell from a 0..1 point in Swift, and the headline, subline, caption and lobby strings are all Swift literals.
4. **The collapsed strip shows a ghost headline.**
   `UtttGameScreen.openness()` is a smoothstep from 270 to 530pt, so at the real compact height (about 330-340pt) the headline is drawn at roughly 18% opacity over the top-right of the board ("Waiting on X", "Your move" are visible in `11`, `12`, `13`).
5. **A black band under the sheet in every drawer screenshot** (bottom ~36pt, the home-indicator safe area).
   The paper stops at the safe area and the host view behind it is unpainted; foolish paints the host view (`tableFallback`).
   The board's grid overshoot also pokes into that band and up through Messages' grabber in the compact drawer.
6. **Expanded screens are mostly empty paper.**
   A tapped bubble opens EXPANDED (Messages' default for a tap).
   The waiting screen expanded is two lines in 830pt (`05`); the game screen expanded has no subline, no turn strip, and 400pt of blank paper.
   The spec's "Waiting" screen has a "Take it back" door; the code has none.
7. **Missing spec surfaces.**
   No "Again" door on the end screen (expanded only, per screen 06/07), no spoken result subline ("Top left, centre, bottom right."), no invitation caption with a name ("Alex wants a game. Tap to take it."), no destination pulse, no settlement half at Send (`didStartSending` is not overridden), no arrival animation for `didReceive`, and undo is instant rather than the 200ms reverse stroke.
8. **Bubble layout is mirrored against the spec.**
   UI.html bubble option 02 is "Board left, state right"; the shipped image puts the text left and the board right.
   The baked "Your move" also reads wrong on the SENDER's own copy of the bubble (the spec accepts one bitmap for both devices, so this needs an owner call, see risks).
9. **Extension Info.plist has no `ITSAppUsesNonExemptEncryption`** (foolish declares it in both; the appex does not inherit).
   No `PrivacyInfo.xcprivacy` in either target.
   No privacy policy or support URL exists for this app.
10. **Stale docs.**
    `uttt/README.md` says "Nothing is built", `uttt/ios/README.md` lists the bubble, sealing and lobby as not done, and `uttt/ios/Tools/rig.env` hardcodes `/Users/alex/Dev/foolish/...` paths.

## 2. foolish solutions mapped to uttt

| Problem | foolish | uttt today | Gap |
|---|---|---|---|
| Seat identity | C `msg_seat_resolve` and friends in `c/src/msg_wire.h`; nickname on the wire; local seat cache in the App Group | Swift `UtttWire.tag` = SHA-256(seed, local participant UUID) per seat, in the URL | Rule in Swift; must move to C. No cache needed because the tag is recomputed from the local UUID. |
| Lobby | `docs/IMESSAGE_LOBBY_V3.md`, C `msg_lobby_*`, Rule P `msg_rule_p` | Swift `present()` switch | Rule in Swift; the spec's joiner-goes-first and double-join tiebreak are unimplemented. |
| Wire | FMSG binary, base32, `https://foolish.cards/m/1<b32>` | Query string, base64url, Swift | Needs a C layout and a C text codec. |
| Staged bubble / send guard / markSent | `MessageComposer`, `stageGeneration`, `didStartSending` -> `sentToken`, `msg_turn_*` state bits | `staged`, `draftURL`, `reverted`; no `didStartSending` | Works for the simple path; no settlement hook, insert errors swallowed. |
| First-paint veil | `msg_turn_publish` raise_veil | none | Low risk here: the board is one cached image, no flights. Not needed for TestFlight. |
| Heights | Fill whatever Messages gives; `CompactRestHeightTests` | Continuous lerp on height | Threshold math puts the headline on the compact strip (defect 4). |
| Captions | `MessageSummary.caption`, strings from C i18n tables via `tools/datagen` | Swift literals in `UtttBubble` | Move to a C string table; add `$<uuid>` name substitution in captions. |
| DEBUG seat picker | `MessageDebugFlags` + `MessageDevBoard.flag(key, shipping:)`, whole files under `#if DEBUG || SOLO_TESTING` | `UtttDev` + `UtttSeatChoice`, whole files under `#if DEBUG` | Equivalent guarantee today. No `flag(key, shipping:)` yet, which is needed once new behaviour is flag-guarded. |
| App Review | `docs/APP_REVIEW_NOTES.md`, `docs/IMESSAGE_SHIP_BLOCKERS.md`, `ios/Compliance.md` | nothing | Privacy URL, support URL, reviewer notes, xcprivacy, appex encryption key. |
| Upload | `appstore-cli-upload` skill, altool with key 33VS3XH2K8 | `uttt/ios/Tools/ship.sh` (new, not yet run end to end) | Needs an ASC app record and store profiles for `cards.uttt.msg` and `cards.uttt.msg.MessagesExtension`. |

## 3. What to move into shared/, and what not to

- **Do not generalise `c/src/msg_wire.c`.**
  Its formats 2-6 live in sealed foolish threads, the body runs to the end of the buffer, and every header field is typed on Durak (`MsgJoin`, variant, rematch, pickup clock).
  Extracting a "generic" header from it risks moving foolish bytes for a two-seat game that needs a fraction of it.
- **uttt gets its own `uttt/c/src/uttt_msg.{h,c}`, shaped like msg_wire** (magic, format byte, versioned refusal, `*_encode`/`*_decode` with negative error codes, and the seat and lobby verdicts as pure functions).
  Rationale: uttt is two seats, no nicknames, no hidden state and no chain, so its wire is about 40 bytes and its rules are about 150 lines of C; sharing would buy coupling, not code.
- **Move to shared/, byte-neutral for foolish:**
  - a text codec for URLs (base32 as foolish uses it, from `fio_b32_encode` in `c/src/replay.c`, plus a C decoder) into `shared/c/b32.{h,c}`; foolish keeps calling it with identical output, proved by its existing tests;
  - `shared/c/sha256.c` is already shared and is what uttt's seat tags should use instead of CryptoKit;
  - the `MessageDevBoard.flag(_:shipping:)` pattern, with the App Group as a parameter, into `shared/swift/DevFlags.swift`;
  - the compliance templates (extension `PrivacyInfo.xcprivacy`, the Info.plist keys) as copies, not symlinks, since each product's bundle must carry its own.
- The rig is already shared through the `RIG_*` block; only `uttt/ios/Tools/rig.env` needs its paths made relative to the repo it sits in.

## 4. Ordered work packages

Each is sized for one agent session of 1-2 hours, runs serially, and ends with a commit.
WP1-WP3 are the TestFlight critical path; WP4-WP6 are spec fidelity and can follow the first internal build.

### WP1 - C owns the wire, the seats and the lobby (critical path)

- New `uttt/c/src/uttt_msg.{h,c}` plus `uttt/c/tests/uttt_msg_test.c` wired into `make run` and `make asan`.
- Binary layout, version 1 (nothing has shipped, so v1 can be redefined now and never again): magic, format, seed (i32), X tag (9 bytes), O tag (9 bytes or absent while open), game code (the existing `uti_encode` bytes).
- The text form is produced and parsed in C (base32 from shared/, or base64url in C); the URL stays scheme-less or becomes an https link, but the string is built by C and Swift only wraps it in `URL`.
- Seat tags: `SHA-256("uttt.seat.1|" seed | participant uuid bytes)` truncated to 9 bytes, computed in C from `shared/c/sha256.c`; drop CryptoKit.
- Per the spec: the creator's tag is written as the open seat's opponent, and **the joiner is X and moves first**; the join bubble always carries the first move.
- Verdict function: `uti_msg_seat(me_tag) -> X | O | OPEN_SEAT_FOR_YOU | WAITING_CREATOR | SPECTATOR | UNREADABLE`, and a tiebreak for two sealed bubbles of one game (lower SHA-256 of replier tag and message payload wins, per UI.html "two people replying at once").
- Tap mapping `uti_hit(u, v) -> move or -1` moves into C; `UtttModel.tap` only forwards.
- Swift: `UtttWire.swift` shrinks to a wrapper over the C entries; the DEBUG `dev:` override stays in Swift but only chooses the bytes fed to the C tag function.
- Done when: `grep -n "SHA256\|base64\|URLQueryItem\|queryItems" uttt/ios` finds nothing; the C test round-trips 10,000 games through encode, text, decode at every ply; every seat verdict has a test; each test is mutation-checked (break the rule, see the named assertion go red, restore).

#### WP1 - done (2026-09-22)

- `uttt/c/src/uttt_msg.{h,c}`: format 1 is magic `0xB7`, format, seed (i32 BE), flags (bit 0 sealed), O tag (the creator, 9 bytes), X tag (the joiner, only when sealed), a 2-byte SHA-256 check, then the `uttt_encode` game.
  The text is `?m=<base32>` (a bare query, since Messages drops unknown schemes); the longest link over 10,000 random games is 87 characters.
- The check exists because a cut-short mixed-radix code decodes into a different game rather than failing; the test flips every bit and truncates at every length, and all are refused.
- Seat tags are `SHA-256("uttt.seat.1|" || seed BE || participant id bytes)[0..9]`, from `shared/c/sha256.c`, pinned by a golden vector.
- The joiner is X and moves first, so taking the seat and the first move are one message: decode refuses an unsealed message with plies and a sealed one without, and `utm_undo` of the joining move unseals.
  This removes the claim-only bubble, so blocker 1 cannot happen.
- Verdicts: `utm_seat` gives X, O, WAITING (my invitation), OPEN (X is mine to take) or SPECTATOR; `utm_can_move`, `utm_play` and `utm_undo` (my own last move only).
  Reinstall follows foolish's exact-or-spectator rule: a sealed game gives SPECTATOR, and an open invitation gives OPEN, the same as for anyone else.
- `utm_prefer(mine, tapped)`: a different game gives the tapped one; a sealed game beats its invitation; more plies wins (Rule P's turn rule); at equal plies on one roster the device's own draft wins; for two joiners the lower `SHA-256("uttt.join.1|" seed, X tag, first move)` wins, and that key is fixed for the life of a fork.
- `uttt_hit(u, v)` (in `uttt_draw.c`, from the same `BL`/`CE` the board is drawn with) and `uttt_active` (one owner for "where next").
- `uttt/c/src/uttt_say.{h,c}`: every bubble, screen, lobby and spectator sentence, with the Swift English kept except the waiting subline, which now follows UI.html ("Nobody has taken it yet.").
  None of the captions uses `$<uuid>` substitution yet, because Swift did not; that is WP4.
- `shared/c/b32.{h,c}`: RFC 4648 base32 with no product name in it, and with a 12-bit accumulator mask (foolish's `replay_b32_*` shifts a signed int without a bound).
  **Left for later:** foolish still has its own copy in `c/src/replay.c`; collapsing it means adding `$(SHARED)/b32.c` to about six foolish build lists (CORE_SRC, IOS_CORE_SRC, l1_measure, WASM_BOT_SRC, rust/Makefile), which was not safe to do without its CI tonight.
- Bridge (`uttt_api.h`): `uti_me`, `uti_msg_open/read/check/text/seat/mark/sealed/seed/can_move/play/undo/prefer/same_game/seat_ids`, `uti_hit`, `uti_say`, `uti_say_mark`, with `UTI_SEAT_*`/`UTI_SAY_*` macros held to the kernel's by `_Static_assert`.
  The resident game is now the resident message's game, `S.seed` and the dead `S.rs` are gone, and every draw fully initialises its display list.
- Swift: `UtttWire` is an opaque kernel string; `UtttModel` taps through `uti_hit` and plays through `uti_msg_play`; the view controller routes on `Uttt.seat` (`.open` opens the board as X with nothing staged until the move); `join()`, `sealedCaption` and the `.start`/`.open` lobby stances are deleted; insert errors are logged.
  `grep -rn "SHA256\|base64\|URLQueryItem\|queryItems\|CryptoKit\|URLComponents\|JSON" uttt/ios uttt/sdk --include=*.swift` finds nothing.
  DEBUG `dev.seat` only picks the identity bytes (`UtttDev.identity`), and a Release build's `strings` still show no `dev.`, "Who are you" or App Group string.
- Tests: `tests/uttt_msg_test.c`, wired into `make run` (10,000 games, 590,089 plies, ~3.6M checks) and `make asan`; `ios-smoke` drives the bridge end to end as three identities.
  All 21 mutations went red on the named assertion: the b32 mask, the check, both roster rules, X != O, the seal on join, the spectator verdict, the unseal on undo, undo ownership, the join-key order, the draft rule, the plies rule, the different-game rule, the tag salt, the `&` stop, WAITING having no mark, the hit clamp and bounds, the em-dash scan, capitalisation, and the headline mark.
  One of them first exposed a test that looped forever when the seal broke; the test now fails instead.
- End to end on a fresh `rig.sh newsim` simulator (screenshots in the session scratchpad `wp1/`, 00-16): create as a, send, tap as b, which opens as X with "Your move", and the first move stages the sealing bubble "Sent to the centre board."; send, tap as a, which opens as O and replies; a change of mind replaces the draft; send; b plays ply 3; the draft's X cancels and reverts it; replay and send.
- Seen along the way and left to WP2/WP4: the picker is not re-asked while the extension stays active (`didSelect`), the ghost headline on the collapsed strip, the expanded screen has no subline, and a cold appex launch shows a blank dark drawer for about 4 seconds before the board.

### WP2 - The join and the lobby work in Messages (critical path)

- Rebuild `present()` on the WP1 verdicts: invitation on open, joiner sees the board with "First move is yours" and the whole sheet washed, their first move stages the sealing bubble, creator's waiting screen gets "Take it back" (clears the draft or, once sent, does nothing destructive).
- Log the `insert` completion error instead of discarding it, and stage only after a transition to compact has settled (foolish: collapse first, then `insertStaged`).
- Request `.compact` when a lobby or waiting screen is opened expanded, since the spec gives the expanded view nothing to show there.
- Add `didStartSending` so the draft is known sent (the foolish `markSent` shape).
- Done when, on a fresh `rig.sh newsim` simulator with `rig.sh picker on`: create as a, send, tap as b, see the draft in the compose field with b's first move, send, tap as a, see "Your move" as O, play, send; repeat three times; every step screenshotted.

#### WP2 - done (2026-09-22)

**Owner decision, over UI.html 02 and the WP2 brief: no undo and no "Take it back".**
The only way to change a move is to tap another square, which replaces the staged draft.
Messages' own X on a draft is system UI and still reverts the board (`utm_undo`, my own last move only).
A take-back message (flag bit, CLOSED seat, door, sentences, screens) was built, played end to end, and then removed from C and Swift on that decision; the waiting screen has no door.

- **Staging, the device bug.** TestFlight 1.0(1) on a real phone never put the invitation in the input field.
  The old code inserted from inside `willBecomeActive`, before `didBecomeActive`, before the view had a window, into a conversation that was not yet active; foolish never inserts that early (its create runs from a tap on a drawer that is already up, and its stage takes `activeConversation`).
  Now an insert waits for both `viewDidAppear` and `didBecomeActive` (with a logged 1.5s deadline if either never comes), goes to `activeConversation`, and from the expanded drawer asks for compact and waits for `didTransition` first (foolish round 10b).
  Every completion is logged (`log stream --predicate 'subsystem == "cards.uttt"'`), a refused insert is retried three times, and a final failure reverts the board as a cancelled draft would.
  A generation counter makes the newest stage win.
  **Not yet proven on a device** - the simulator staged before this fix too.
- **Send and cancel.** `didStartSending` records what was sent from the message Messages hands over (`markSent`, which refuses to rebase backwards onto an older bubble of the same game, by `utm_prefer`); `didCancelSending` acts only on the current draft, re-reads it, undoes my own move (a cancelled join gives the seat back), or closes the drawer when the cancelled draft was an unsent invitation.
  The first send from a drawer opened through `+` closes it (foolish's unbound-drawer finding), a send from expanded closes it, a send from compact keeps the strip up.
- **Tap a bubble while open.** `didSelect` of a different bubble re-presents that game (the tapped game wins over this device's own newest when they are different games), and in DEBUG re-asks the seat picker; our own insert moving the selection is ignored.
  Tapping the bubble that is already the selection sends no callback at all (measured), so the rig closes the drawer between seats.
- **Game over.** The end subline speaks the winning line ("Top left, centre, bottom right.", `uttt_won_line`), the caption names it ("X won on the diagonal. 17 moves."), and a finished game offers **Again** in the expanded view only (`utm_door`), to anybody.
  Again stages a fresh invitation in a NEW MSSession, so the finished game's last bubble stays in the thread, and the proposer moves second.
- **Cold launch.** Filmed (`rig.sh film`) and logged with process uptime.
  Before: tap in the `+` menu to paper 4.0s, and in one take 8s with the drawer vanishing and a full-screen sheet of paper flashing for 1.3s while the insert landed.
  Cause of the flash: the extension's view is first laid out at the whole window (440x956) and shown at that size until the compact transition; nothing is attached now until the first drawer-sized layout.
  After: tap to paper 3.4s, no flash, and paper is the first frame our process draws (about 50ms after the drawer has a size); the first board of a process renders off the main thread (CoreGraphics fill of a 14,257-polygon board measured 140-250ms Debug, the kernel's draw 0.5ms) and lands a few frames later.
  The rest is before `viewDidLoad`: `sample` of the launch puts 2.2s of 2.9s in `_accessibilityInit` loading accessibility bundles, which the simulator does because the rig's accessibility automation is on; with it off, `viewDidLoad` came at 1.9s.
- **The paper** fills the drawer edge to edge in both heights (the host view and the safe area are painted; `UtttSheet` ignores the safe area and clips its content), so the black band is gone and the board's overshoot no longer runs up through the grab handle.
  The collapsed strip no longer draws the ghost headline (the openness window now starts above 340).
  The board image cache is keyed on the game itself, not a counter that every new model restarts at.
- Evidence: screenshots, films and contact sheets in the session scratchpad `wp2/` (cycles `60`-`80`, the full game by taps `100`-`118` and `140`-`153`, cancel `22`, `44`, `132`, change of mind `130`-`131`, tap while open `90`-`92`, Again `119`-`120`, `153`).
- Tests: `uttt_won_line`, `utm_door` and the new sentences in `tests/uttt_msg_test.c`, each mutation-checked red on its named assertion; `make -C uttt/c run asan ios-smoke` green; Release device build clean and its `strings` still show no `dev.` file, picker text or App Group.

Left from WP2:
- Prove the staging fix on a real phone (the one thing the simulator cannot show).
- The first-board async render is not flag-guarded (`feedback_flag_guard_new_changes`); uttt has no `flag(_:shipping:)` yet (WP6).
- The drawer's compact headline, subline on your own turn, bubble layout (board right, text left) and "Your move" on the sender's copy are WP4.
- Dismissing the drawer re-lays the view at full window size once more (a 414-point raster is logged at `resign`); not seen on film, not investigated.

### WP3 - Store plumbing and the first internal TestFlight build (critical path)

- Add `ITSAppUsesNonExemptEncryption = false` to `uttt/ios/UtttMessages/Info.plist`.
- Add `PrivacyInfo.xcprivacy` to the extension (tracking false, empty domains, data types and API types; copy `ios/FoolishMessages/PrivacyInfo.xcprivacy`).
- Add the classic AppIcon ladder to the container (40/58/60/80/87/120/180 via `icons.sh`), per the foolish note that a 1024-only set ships a loose 1024 copy and loses thinning.
- Publish `public/uttt-privacy.html` and `public/uttt-support.html` (static, no JS; "Data Not Collected": the game is ~22 bytes in the message URL and nothing leaves the device otherwise) and deploy them the way `public/imessage-privacy.html` deploys.
- Owner, in the portal (cannot be done by the API): the app record for `cards.uttt.msg`, the explicit bundle id for `cards.uttt.msg.MessagesExtension`, and the store name (the display name "Ultimate" is likely taken on the store; pick the listing name).
- Run `uttt/ios/Tools/ship.sh` and wait for VALID; add the build to an INTERNAL TestFlight group (internal testers need no Beta App Review, which is what makes "tonight" possible).
- Done when: ASC lists the build as VALID, `PlistBuddy` on the archive shows `cards.uttt.msg` with both encryption keys, and the build installs from TestFlight on a real phone and appears in the Messages `+` drawer.

### WP4 - Screens to the spec

- Fix the ghost headline: at compact heights the headline must be fully transparent, or better, derive the compact and expanded ends from the two heights Messages actually reports rather than a fixed 270..530 window.
- Paint the host view and the safe area in paper, so the bottom band is gone; clip the grid overshoot to the sheet, not to the screen edge, and keep it out of Messages' grabber.
- Expanded: add the subline under the headline ("Anywhere you like.", the place name), the end-screen result line and the "Again" door (expanded only), per UI.html screens 04-08.
- Bubble: board left, state right (UI.html option 02), last mark heavier (already true), and captions that name the mover with Messages' `$<participant uuid>` substitution where the spec uses "Alex".
- Move every user-visible string into a C table (`uti_text(key)`), the same way foolish's i18n tables work, even if English only for now.
- Done when: a screenshot of each UI.html screen (01-08, collapsed 340 and expanded) is taken from the rig and laid next to the spec frame, and nothing in the frame is unexplained.

### WP5 - Motion channels

- Destination pulse at +300ms after the ink lands (channel A), auto-collapse once the ink lands and never during, settlement (big mark, meta line) at `didStartSending` (B), replay on reopening my own bubble without pulse (C), both halves plus pulse on their bubble (D), arrival on `didReceive` (E), and the 200ms reverse un-ink on undo.
- The timings live in C next to the pen (a `uti_motion` table), Swift only asks for `t`.
- Done when: `rig.sh film` takes of each channel are measured with the animation-measure skill and match UI.html's Motion tab timings.

### WP6 - Flag guard, docs and rig hygiene

- Add `shared/swift/DevFlags.swift` (`flag(_:shipping:)`, App Group as a parameter) and use it for every WP4/WP5 change that ships default-on.
- Add a Release gate script: build Release, `strings` the binaries, fail on `dev.` file names or the App Group string.
- Rewrite `uttt/README.md` and `uttt/ios/README.md` to the true state; make `uttt/ios/Tools/rig.env` derive paths from its own location.
- Done when the gate goes red on a planted `dev.seat` reference in a non-DEBUG file, and green once removed.

## 5. Risks

- **Two real devices were never used.**
  The simulator seat override proves the flow, not Apple's participant UUID behaviour; the first TestFlight build must be played between two phones before anyone else sees it.
- **Reinstall makes a player a spectator in their own game** (the tag is a hash of a per-install UUID).
  Acceptable for TestFlight; the fix is a keychain secret and is deliberately out of scope.
- **Group-chat double join** is not implemented today; until WP1 lands, two joiners can both believe they are seated.
- **The baked bubble says "Your move" to the sender too.**
  One bitmap serves both devices; the spec's frame 02 shows "Your move", so this needs an owner call (e.g. a drawn mark: "O to move").
- **Names cannot be drawn into the bubble image**, only into captions via `$<uuid>`; the spec's "Alex takes it" in the image is not achievable as drawn.
- **`rig.sh build` runs `git checkout --` on every tracked `*.entitlements` in the repo**, which throws away uncommitted entitlement edits by any agent sharing the worktree.
- **`ship.sh` has not been run end to end**, and the store profiles for the two uttt bundle ids may not exist yet.
- **External TestFlight needs Beta App Review** (hours to a day) and a working privacy URL; tonight is realistic only for the internal group.
- **App Review 4.2 (minimum functionality) and naming**: "Ultimate Tic-Tac-Toe" is generic; the README already recommends a distinct app name.
- **`uttt_bots.o` is inside the shipped static library.**
  It is dead-stripped only if nothing references it; check the Release binary size and symbols after WP1.

## 6. Build, run and rig from this worktree (commands that worked)

```bash
cd /Users/alex/Dev/foolish-uttt
make -C uttt/c run asan ios-smoke
make -C uttt/c ios-lib
(cd uttt/ios && xcodegen generate)
xcodebuild -project uttt/ios/Uttt.xcodeproj -scheme UtttMessagesApp \
  -destination 'generic/platform=iOS Simulator' -derivedDataPath <scratch>/dd build
xcodebuild -project uttt/ios/Uttt.xcodeproj -scheme UtttMessagesApp -configuration Release \
  -destination 'generic/platform=iOS' -derivedDataPath <scratch>/ddrel CODE_SIGNING_ALLOWED=NO build
```

The rig, with the worktree overrides after sourcing:

```bash
source uttt/ios/Tools/rig.env
export RIG_XCPROJ=/Users/alex/Dev/foolish-uttt/uttt/ios/Uttt.xcodeproj \
       RIG_KERNEL_DIR=/Users/alex/Dev/foolish-uttt/uttt/c \
       RIG_IOS_DIR=/Users/alex/Dev/foolish-uttt/uttt/ios
eval "$(ios/Tools/rig/rig.sh newsim UtttAudit)"   # this audit: 611A9A19-F55C-481E-9B70-6440E49C3BFC
ios/Tools/rig/rig.sh build
ios/Tools/rig/rig.sh stage
ios/Tools/rig/rig.sh enter
ios/Tools/rig/rig.sh open          # prints "no 'Ultimate' on screen" but the drawer does open
ios/Tools/rig/rig.sh picker on     # or: rig.sh seat a|b between taps
ios/Tools/rig/rig.sh devgame 12    # straight to a seeded board; rig.sh killappex then open to switch seats
```

Taps were done directly with `idb ui tap --udid $FOOLISH_SIM X Y` in points (a 1320x2868 screenshot pixel divided by 3).
`rig.sh collapse` and `rig.sh back` failed on this product ("no drawer on screen", "could not open conversation '888'"); an `idb ui swipe` from the grabber down works instead.
