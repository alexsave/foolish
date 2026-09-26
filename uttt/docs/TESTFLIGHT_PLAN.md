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

#### WP4 - done, first pass (2026-09-23)

Spec-vs-app sheets are in the session scratchpad `wp4/` (`sheet_1.png` bubble, `sheet_2.png` collapsed, `sheet_3.png` expanded, `sheet_4.png` waiting, `sheet_5.png` screen 04), with the UI.html renders in `wp4/spec/` (headless Chrome, `UI.html#bub2`, `#coll2`, `#exp2`, `#lobby2`).

- **The bubble never says "Your move".**
  It is one bitmap on both phones, so its headline is now the side to play as a DRAWN mark in its own ink plus words: "<O> to play", "<X> wins", "A game?", "A draw" (`uttt_say_bubble_mark`, `UTTT_SAY_BUBBLE_HEADLINE`).
  The place line stays blue under it, as in option 02.
- **The board stays on the RIGHT, mirrored against option 02, on purpose.**
  Messages stamps the app's logo badge (about 31x24pt, 6pt in) into the top-left corner of every template bubble, sent or staged; with the board on the left it would sit on the top-left cell of the top-left block for the whole game.
  A staged draft also carries Messages' X in the top-right corner, but only on the sender's own draft.
  If the owner wants the spec anyway, it is `b.board.x` and `b.text.x` in `uttt_bubble()`.
- **"Clipped at the right edge" was the grid's 13.5% main-line overshoot running into the bubble's edge.**
  The bubble now draws with `UtttDrawOpts.reach` = 5/13.5 (UI.html's own 5%) through `uti_draw_bubble`, and the board is 170pt (not 181) so the longest line stops 3pt or more inside the frame on the three sides it faces (asserted in `ios-smoke`).
  The drawer keeps the full overshoot.
- **Bubble type is 18pt**, measured off option 02 ("Your move" is 82pt wide there); it was 16.
- **Captions name nobody yet.**
  `$<participant uuid>` in `MSMessageTemplateLayout.caption` and `summaryText` is NOT substituted on the iOS 27 simulator: the raw `$FEACEE0B-...` showed in the draft, the sent bubble, the incoming twin and the conversation list.
  The kernel can word both named captions (`uttt_say_by`: "<who> wants a game. Tap to take it.", "<who> won on the diagonal. 58 moves."), and Swift passes no name until two real phones show Messages substituting it.
  The invitation caption is now "A game. Tap to take it." (was "New Ultimate Tic Tac Toe game").
- **Compact strip**: matches "Collapsed 340" (you-are column, board centred, no headline); nothing was missing.
  The waiting strip's words take their own width, so "Nobody has taken it yet." is one line (it broke after "taken").
- **Expanded**: matches "Expanded" (you-are and mark top left, headline top right, rulebook door bottom right) plus the subline from Lobby 04/06/07; "Anywhere you like." verified on the join.
  No take-back door (owner).
- Tests: new assertions in `tests/uttt_msg_test.c` and `ios/uttt_api_smoke.c`; four mutations (reach ignored, name ignored, bubble mark forced to 0, invitation sentence cut) each went red on its named assertion.
  `make -C uttt/c run asan ios-smoke` green; Release device build clean, and `strings` shows no `dev.seat`, picker text, App Group or `$<uuid>`.
- Cycle re-run on a fresh sim: create as a, send, tap as b, the first move stages the sealing bubble, send, tap as a, the reply stages, send.

Left from WP4:
- Prove or drop `$<uuid>` caption substitution on two real phones.
- The expanded board's main lines still run off the sheet's left and right edges (the kernel's deliberate 13.5% overshoot, about 55pt on a 411pt board); UI.html's frames show them stopping at the board.
  Owner call on the pen.
- Game-over and spectator screens were not re-shot this round (WP2 shot them, and no code on those screens changed).

### WP5 - Motion channels

- Destination pulse at +300ms after the ink lands (channel A), auto-collapse once the ink lands and never during, settlement (big mark, meta line) at `didStartSending` (B), replay on reopening my own bubble without pulse (C), both halves plus pulse on their bubble (D), arrival on `didReceive` (E), and the 200ms reverse un-ink on undo.
- The timings live in C next to the pen (a `uti_motion` table), Swift only asks for `t`.
- Done when: `rig.sh film` takes of each channel are measured with the animation-measure skill and match UI.html's Motion tab timings.

#### WP5 - done, first pass (2026-09-23)

Evidence (films, frames, probe CSVs, screenshots) is in the session scratchpad `wp5/`; `motion_probe.py` and `score.py` there turned each film into numbers.

- **The kernel owns the motion.** `uttt/c/src/uttt_anim.{h,c}`: `uttt_motion(game, channel)` plans the last move and `uttt_motion_at(plan, now_ms, &frame)` is a pure function giving the ink's progress, the highlighter's rect and alpha, the ring, `landed` and `settled`.
  Channels: A stage, C my replay (no pulse), D their bubble, E arrival; `UTI_CH_OPEN` lets the kernel pick C or D from the seat.
  Timings from UI.html's grid: X inks in 260 ms and O in 340 (on the page's own cubic-beziers), the wash travels AFTER the ink lands (340 ms mine, 420 theirs, smoothstep, one rect sliding and resizing, growing to the sheet when freed), and the ring opens 300 ms after the ink lands, 620 ms, twice (the page's cellpulse keyframes).
  Undo and the settlement half at Send (B) are not animated: no undo (owner), and B is left (below).
- **Swift runs one display link and draws** (`UtttMotionClock`, `UtttLiveBoard`); it types no duration.
  The cache is now the board WITHOUT the last mark and without the wash (`uti_draw_under`), so a frame draws the wash rect, the cached image, the ring and the last mark (`uti_draw_last`, a few hundred polygons); `under + last == board` is a C test.
  The clock starts on the first frame the board image is ready, so a cold open does not play its ink under a blank board.
- **The bubble is painted off the main thread** (`UtttBubble.snapshot()` on main, `image(snapshot)` anywhere) and inserted only once the board has `settled`.
  Before this the 14k-fill bubble paint froze the main thread for ~190 ms at every stage, and the highlighter jumped instead of travelling (take_stage1).
  From the expanded drawer the paint and the collapse now run together; waiting for the transition after the paint had missed it and sat out the 1.2 s timeout.
- **The main lines stop at UI.html's 5% in the drawer too** (`UTTT_REACH`, one constant for bubble and drawer), and the board's width leaves room for that overshoot (`uti_board_reach`), so the expanded lines end on the sheet as in the spec (`sheet_wp5_layout.png`).
- **Measured at normal speed** (sim, 60-75 Hz): stage take 5: ink visible to 95% in 183 ms, wash travel 318 ms visible, worst frame gap 22 ms, max deviation from smoothstep 0.07; the kernel logged `motion done 1823 ms` for a 1800 ms plan.
  Their bubble (D) cold open: ink 195 ms to 95%, wash 448 ms for a 420 ms plan, worst gap 50 ms, deviation 0.06, `motion done 1832 ms`.
  Stage take 4: travel clean, then a 90 ms stall during the first ring when Messages inserted the bubble.
- Tests: `tests/uttt_anim_test.c` in `make run` and `make asan` (32 checks); 9 mutations (wash with the ink, pulse on replay, reach 1, last mark dropped, pulse at 0, the X and the O ink curves each linear, wash from the destination, settled at landing) each went red on the named assertion.
  `make -C uttt/c run asan ios-smoke` green; Release device build clean, `strings` shows no `dev.seat`, picker text or App Group.
- Cycle re-run on the sim: b plays and stages from compact, sends; a opens the bubble expanded (D plays), replies, the drawer collapses and the reply stages at once, sends; the older bubble folds to its caption (`sheet30.png`, `sheet33.png`).

Left from WP5:
- The settlement half at Send (B: the big mark and the win line drawn in at `didStartSending`); the kernel draws a won block's big mark at stage today.
- A stage from EXPANDED: the collapse plus a re-raster at the compact size (60-80 ms on the main thread) leave the motion ~14 frames over 2 s; the next raster could go off-main like the first.
- From tap to the first ink frame is one sync raster of the new position (~60 ms Debug); from ink landing to `stage` logged ~250 ms in Debug, not yet profiled.
- Expanded layout: the board sits centred, the spec has it high (top at ~144 of 830 points) with the you-are mark centred above it; not changed.
- Game-over and spectator screens were not re-shot (spectator uses the static board, no motion).
- The ring is drawn but the probe only catches its first ~100 ms reliably (its colour fades into the paper); confirm by eye on a device.

#### Verification pass (build 3) - 2026-09-23

Run against 1.0(3) (HEAD `9d499dd2`) on a fresh `rig.sh newsim UtttVerify` simulator, Debug with the picker and a Release simulator build.
Screenshots are in the session scratchpad `verify/` (`/private/tmp/claude-501/-Users-alex-Dev-foolish/ce7c4549-2c3c-4e25-aeb1-0540c12ff110/scratchpad/verify/`).

What held up:
- A whole game by taps from a cold start (`01`-`07`, `p1_*`-`p21_*`): + drawer, invitation staged and sent, joined as X with the first move, 21 plies alternated through the picker, X won down the left, the loser's end screen reads "X wins" and the winner's "You win".
  Again staged a new invitation in a new session, the finished bubble stayed in the thread, and the second game's join and first move worked (`20`-`26`).
- Change of mind replaces the draft, and Messages' cancel X reverts the board (`85`, `86`).
- A tapped cell in the (moved) expanded board lands where tapped (`83`, `84`).
- Release simulator build: + opens the invitation staged, send, tap own bubble gives Waiting from the device's own participant id (`70`-`73`); no `dev.seat`, picker text, App Group or em dash in the Release binaries.
- The paper is the same sheet in dark and light appearance, bubble and drawer (`50`-`63`).
- `make -C uttt/c run asan ios-smoke` green after every change.

Fixed in this pass:
- `39ec7770`, `45cffd68` - **the Again door is drawn by the pen** (owner): `uttt_draw_door` in `uttt_rule.c` is a rough outline over the rulebook square's two-fifths hachure, in points so a wider phone gets more hachure at the same gap; the label is set in the outline's ink; its height is `UtttRulebookButton.expandedSide`, the rulebook door's; its edge is 2.6 wide so the top and bottom read as a box.
  ios-smoke asserts it fits at every width 200-430, the two inks, fill before outline, no stray point, determinism, and more hachure when wider; the shape buffers grew from 96/2400 to 256/6400 because the first run of that test went red (only 96 strokes at every width).
  Mutations (fill ink, edge painted first, drifting seed, scale-free gap, inset outside the bar, buffers back to 96) each went red on the named assertion.
- `7c91cc90` - **captions and screen lines end without a period** (owner); the end caption is "X won down the left in 21 moves"; a draw is "Drawn in N moves"; the end subline names the line ("Left column", "Top row", "Diagonal" for both, matching the caption's "on the diagonal").
  `uttt_msg_test` asserts no string from any key at any ply from any seat ends in a period, the subline per line, and that caption and subline name the same line; three mutations went red.
- `45cffd68` - **the expanded board sits high** (the WP5 gap): board top 30 points under the bar, spare height at the bottom with the doors; the you-are mark stays under its label (owner, over UI.html's centred mark). Collapsed is unchanged, still one lerp.
- `8184b270` - **the spectator's expanded board ran its main lines off both edges of the sheet** (it took the full width); it now leaves room for the 5% reach and sits high.
- `118361b2` - **the same on the expanded Waiting screen**, which is the first thing a creator sees on tapping their own invitation.
- `4ff6abdc`, `581321db` - rig only (DEBUG): `rig.sh devgame 47,20,...` opens an exact game, and the seeded path routes by seat, so won, lost, drawn and spectator states can be shot.

Open (all but the recording closed in the polish pass below):
- **App Review, one device (major, before external TestFlight or the store):** a reviewer creates a game, sends it, taps it and sees "Waiting / Nobody has taken it yet" with nothing to do.
  There is no way to see a single move without a second Apple ID in the conversation.
  The review notes must say so plainly and ship a screen recording of a two-phone game (the foolish pattern in `docs/APP_REVIEW_NOTES.md`); there are no uttt review notes yet.
- **The collapsed end screen has no verdict** (minor): UI.html 08 shows "Alex takes it" above the collapsed board; the strip shows the board only (`owner_collapsed_youwin.png`). Owner is looking at it.
- **The spectator has no rulebook door** (minor); every other screen has one.
- **The waiting screen opens EXPANDED from a tap** and shows two lines over an empty board (WP2 asked for a request to compact); cosmetic.
- **VoiceOver:** the board and its cells have no accessibility labels (the Again door and the rulebook now do); nice-to-have.
- The folded line above the newest bubble shows the FIRST move's caption for the whole game ("Sent to the top-right board." stayed through 21 plies); that is Messages folding an MSSession, not our text, and was left.
- Not re-proven here: two real phones, `$<uuid>` captions.

A new build is warranted: the door, captions, subline and the expanded layouts are all user-visible changes since 1.0(3).

#### Polish pass (after build 3) - 2026-09-23

Shots are in the session scratchpad `polish/` (`/private/tmp/claude-501/-Users-alex-Dev-foolish/ce7c4549-2c3c-4e25-aeb1-0540c12ff110/scratchpad/polish/`), from a fresh `rig.sh newsim UtttPolish` (iPhone 17 Pro Max, 440 wide) with `devgame` on the diagonal fixture from `uttt_msg_test.c`.

Closed:
- **The collapsed end strip carries the verdict** (UI.html 08, "the verdict and the board"): "You win" / "<X> wins" / "Drawn" with the line name under it ("Diagonal"), top right, where the expanded sheet puts it, so a collapse is still one lerp.
  The board gives up the verdict's 50 points on the strip only when the game is over; a live game's strip is unchanged.
  `owner_collapsed_youwin_after.png` is the owner's shot redone; `collapsed_xwins_dark.png` is the loser's, `collapsed_youwin_light.png` the light twin.
- **The spectator has the rulebook door**: 38 points in the right column on the strip (the board now leaves the play surface's two 38-point columns), beside Again at 54 when expanded, and the rules open on the same sheet with Back (`spectator_*`).
- **VoiceOver** reads every square ("Top left board, top middle square, O"), the headline with the drawn mark spelled ("X wins", "Waiting on O"), "You are X", and the doors ("Again", "Rulebook"); only a square the player may take is a button, and activating it is the same tap.
  The words are `uttt_say` keys (`HEADLINE_SPOKEN`, `YOU_ARE_SPOKEN`, `DOOR_RULES`) and `uttt_say_cell`; each element's rectangle is `uttt_cell_rect`, the inverse of `uttt_hit`, so Swift places and labels and computes nothing.
  Read back from the running app with `idb ui describe-point`.
  `uttt_msg_test` and `ios-smoke` assert them; four mutations (X/O swapped on a square, the rectangle's row from the wrong digit, the mark left out of the spoken headline, the wrong side in "You are") each went red on the named assertion.
- **The waiting screen opening expanded is the spec, not a defect.** UI.html 02 ("Waiting, as the one who asked") is drawn at the expanded 620-point sheet, and foolish shows its own lobby bubble expanded on a tap the same way; Messages presents a tapped bubble expanded, and asking for compact straight after would be an expand-then-collapse bounce on every tap. No code change; WP2's "request compact" line is withdrawn.
- **Review notes**: `uttt/docs/APP_REVIEW_NOTES.md`, pasted into the live Beta App Review notes. It explains the one-device "Waiting" screen, the two-Apple-ID test, the game, no accounts/network/purchases and the missing home screen icon, and records the one-device options considered (none built; the seat picker stays DEBUG-only).

Still open:
- A screen recording of a two-phone game to attach for review (needs two real phones).
- The folded MSSession caption line (Messages' text, left as is).

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

## 7. Drawer motion (ruler)

Measured on 2026-09-23 on UtttRig (iPhone 17 Pro Max, iOS 27 simulator), seeded board `devgame 12`, seat b, normal speed, no slow-mo.

### The instrument

`touch dev.ruler` in the App Group (`rig.sh ruler on`) paints the ruler over the sheet in a DEBUG build.
The generic half is `shared/swift/MotionRuler.swift`: the dev-file reader, the square palette, the red top and green bottom edge bars, the banded strip and a millisecond clock strip.
`uttt/ios/UtttKit/UtttRuler.swift` lists where each square sits: magenta board centre, cyan board corners, orange you-are mark, yellow headline, lime subline, blue rulebook door, violet Again door and pen stroke, pink highlighter.
With the ruler on, every height the sheet is handed is logged as `ruler-height <h> clock <ms>`, so a log line can be matched to the filmed frame showing that clock.
A Release build contains none of it: `strings` on all three Release binaries finds no `dev.*` file name, no `ruler-height`, no App Group string and no em dash (only the mangled names of the no-op stubs).

The readers are `shared/rig/lib/marks.py` (every square by hue, the bars, the clock, per frame, to CSV), `ride.py` (does each mark ride the drawer: snaps of its offset from the red bar above 4pt, late snaps after the drawer stopped, second-difference roughness, missing frames) and `marksplot.py` (each mark's y and its offset from the red bar over time).

### What it found and what changed

**Auto-collapse re-laid out after the drawer stopped.**
Messages hands the sheet the compact height once, before the slide, inside a UIKit animation block, and the hosting controller bridged that animation into SwiftUI.
The sheet then crept on the host's curve while the drawer slid and landed in steps at +240, +370, +530 and +650ms, three of them after the drawer had settled.
Fix: a new height is laid out at once (`.transaction(value: height) { $0.animation = nil }` on the sheet; the ruler's own bars likewise).
Every mark now makes one move at the start of the slide and then rides the drawer flat to the end.

| auto-collapse, per mark | before (3 light takes) | after (3 light, 2 dark) |
|---|---|---|
| snaps after the drawer stopped | 1.0-1.3 per mark per take | 0 for every mark |
| board centre | 3.0 snaps, largest 113pt | 1 snap (the start resize), then flat |
| board corners | 1.7-3.0 snaps, largest 78pt | 1 snap, then flat |
| rulebook door | reappears in 3 steps | 1 snap, then flat |
| you-are mark | 1 snap, 6pt | 1 snap, 8pt |

**A resized board was painted on the main thread.**
Every new side cost 50-60ms of main thread (`raster side N` in the log), and a manual drag hands a new height every frame, so the board moved in ~200ms steps under a finger moving every 16ms.
Fix: the same board at a new size draws the picture on hand scaled into the new rectangle and paints the sharp one off the main thread, one paint at a time, following the latest side.
Manual drag collapse at 0.6s, one take each: largest snap board centre 73 -> 55pt, lower corners 100 -> 69pt, door 124 -> 90pt, roughness down 13-40% per mark.

**The layout followed the sparse heights Messages hands, not the drawer (second pass).**
Messages hands one height about 20ms before an auto-collapse slides, and after a released drag a new one only every ~200ms, so the board shrank in one frame and then in 50-100pt steps.
Fix: the sheet is laid out at the kernel's drawer height (`uttt_drawer_*` in `uttt/c/src/uttt_anim.c`, bridged as `uti_drawer_*`), a critically damped spring on the host's response (0.338s, docs/COLLAPSE_MSE.md) from where the layout is toward the last height handed.
A change within 32pt with no spring running (a finger on the handle) is followed at once; anything else re-aims the spring from its current position and velocity, and a spring from rest waits the 20ms lead.
`UtttDrawerClock` owns only the display link; a layout pass asks `uti_drawer_peek` so the frame that first sees a new height never draws it raw.
Tests: `uttt_anim_test` (continuity of position and velocity, no overshoot, the follow, the lead; every check mutation-checked) and `ios-smoke` (the peek).

**The board was re-rendered on the main thread every frame of a resize.**
A `sample` during an auto-collapse put 276 of 1111 main-thread samples in `RBLayer display` under the board's Canvas, with a vImage RGBA-to-BGRA permute at the top of the stack.
Fix: the cached board is an `Image` view (a texture the compositor scales), painted in BGRA, the compositor's own layout; the Canvas keeps only the wash, the ring and the moving stroke.
Same measurement after: 21 samples in `RBLayer display`, no permute.

Per mark, largest snap of its offset from the drawer (pt), before -> after; UtttRig, light, normal speed:

| mark | auto-collapse (3 -> 3 takes) | drag collapse 0.6s (2 -> 2) | drag expand 0.6s (2 -> 2) | flick collapse 0.15s (1 -> 1) |
|---|---|---|---|---|
| board centre | 156 -> 74 | 77 -> 58 | 64 -> 54 | 85 -> 0 |
| lower corners | 205 -> 44 | 103 -> 0 | 83 -> 72 | 111 -> 0 |
| upper corners | 107 -> 53 | 49 -> 48 | 53 -> 36 | 62 -> 0 |
| rulebook door | 538 -> 34 | 129 -> 22 | 200 -> 149 | 158 -> 0 |
| highlighter | 191 -> 61 | - | - | - |

Roughness (sum of squared second differences) fell for most marks, e.g. auto-collapse board centre 87087 -> 33203, drag expand board centre 50638 -> 29710.
Charts: `pass2_*.png` beside the earlier ones in the ruler scratch folder.

### What is left

- **The auto-collapse still moves in 2-3 steps of up to ~75pt.** The ruler clock shows the extension committing a frame only every 40-50ms during the slide on the simulator; the main thread now waits on the simulator's Metal (`waitUntilScheduled`), not on our code.
  This needs a device take before anything else is changed.
- **During an auto-collapse the layout lags the drawer**, so the board's lower edge and the door run below the screen for ~16-24 frames (counted as misses); the spring could start later or run faster, but only a device measurement can say which.
- **Drag expand improved least** (door 200 -> 149pt): the first ~200ms of an upward drag also arrives as sparse ~120pt heights, which the spring now smooths but trails.
- Not measured after the fix: slow drags (1.5s) and the flick up (the takes did not register a drag), tap-to-expand, dark mode, and the smallest phone width.
- Still one to three takes per variant, not the 6 to 20 the method wants.
- The top-left corner square is not found in compact takes (it sits against the you-are mark), so it is scored expanded only.

### One layout for every screen, the board holding the centre (2026-09-23)

Owner: "the game isn't centered in the collapsed view anymore" and "in the first open screen (from +) the board DOESN'T smoothly transition when I manually drag, it jumps from small to large".
Both measured with the ruler on UtttRig before and after, manual drags at 0.6s, normal speed; takes, charts and screenshots are in the session scratchpad `center/`.

What was wrong:
- `UtttLobbyScreen` (the waiting screen, which is what `+` opens) and `UtttWatchScreen` had two layouts and a switch at 440 points (`UtttDoorButton.expandedFrom`), and neither used the drawer clock.
  Dragging across it jumped the waiting board from beside the words to under them: 198 to 377 points in ONE frame, while the drawer moved 21.
  On the strip its board sat 80 points right of the sheet's centre.
- The play surface was one lerp, but the expanded board was lifted to sit "high, just under the header" (verification pass, `45cffd68`), and the end strip pushed the board down under the verdict.
  UI.html's "What holds which edge" says the board holds THE CENTRE ("348 to 214 is a scale, not a slide"), and its own expanded frames centre it between the bar and the doors (`margin:auto`), so the lift is withdrawn.

What changed:
- `uttt_sheet` in `uttt/c/src/uttt_anim.c` (bridged as `uti_sheet`, Swift `Uttt.sheet`) is the one owner of every screen's geometry: the board's centre is the sheet's centre at every height, its side a continuous function of the drawer height (min and max of lerps on the smoothstep openness, no branch on it), and everything else is placed at an edge around it.
  A screen's words at the top (the waiting lines, the verdict, the spectator's line) are a box Swift measures; the board gives up only what it must to clear that box, beside it when narrow enough, under it otherwise, and the same room at the bottom so the centre stays the centre.
- `UtttDrawerSheet` is the one container all three screens lay out in: the drawer clock's height, laid out at once, with the ruler's edge bars. No screen can lay out at the raw height or branch on it; `expandedFrom` is gone.
- The waiting and spectator boards carry the ruler's five squares (`boardRuler()`).
- Tests: `uttt_anim_test` sweeps heights 220-900 at a quarter point, three widths and four screens: centred to 1e-3 at every height, side step at most 0.25pt per 0.25pt of drawer, lines on the sheet, clear of the strip's words, and the live strip hides the headline; `ios-smoke` checks the bridge.
  Seven mutations (the old top lift, an x offset, a threshold at t 0.5, words ignored, overshoot ignored, headline always shown, a top-only reserve) and one bridge mutation each went red on the named assertion.

| take (0.6s drag) | largest one-frame side step, before -> after | worst board cx - screen cx | board cy - sheet cy at rest |
|---|---|---|---|
| first-open, expand | 167.7pt (sheet moved 21.3) -> 20.7pt (sheet 21.7) | 80.0 -> 0.3pt | -147 -> 0.0pt |
| first-open, collapse | 167.3pt (sheet 40.7) -> 20.0pt (sheet 20.3) | 80.0 -> 0.3pt | +0.2 -> +0.2pt |
| in game, expand | 37.7pt (sheet 93.7) -> 31.3pt (sheet 104.3) | 0.4 -> 0.4pt | -119 -> 0.0pt |
| in game, collapse | 20.0pt (sheet 80.7) -> 27.7pt (sheet 100.3) | 0.3 -> 0.3pt | +0.2 -> +0.2pt |

After the change the first-open board's side moves one-for-one with the sheet every frame.
The in-game steps are frames where the simulator committed nothing for a while and the drawer moved 80-100pt meanwhile (the known sim frame pacing, "What is left" above); the board's step is smaller than the drawer's in every one.
During those drags the layout trails the drawer by up to ~70pt (the drawer spring, unchanged here).

Compact screenshots (UtttRig drawer 440x274-278), board centre x vs screen centre 220 and y vs sheet centre: waiting +0.0/+0.2 (board 165), live game +0.0/+0.2 (268), end as winner +0.0/+0.2 (230), end as loser +0.0/+0.2 (223), spectator +0.0/+0.2 (213); expanded, every screen -0.3/0.0 (371).

Conflicts, kept centred per the spec:
- The waiting strip's words ("Nobody has taken it yet", about 165pt) cannot sit beside a centred board, so the board goes under them and gives up the same room at the bottom: 165pt on this simulator's 274pt strip (was 198, off-centre), about 224pt at UI.html's 340.
- The end strip's verdict sits beside the board, which gives up 38-45pt (230/223 against 268 live).
- The spectator's line spans the sheet, so its board is 213.

## 8. The send hint, the insert watchdog and the biggest board (2026-09-23)

Verified on UtttRig (iPhone 17 Pro Max, iOS 27 simulator, light and dark); shots in the session scratchpad `hint/`.

### The send hint, shared with the sister product

The staged-but-unsent reminder (the bobbing `arrow.up` in Messages' send blue under the Send button, its caption, the send axis and the white ring) moved out of the sister product into `shared/swift/MessagesKit/SendHint.swift` (`SendHint`, `SendHintArrow`, `SendHintRing`).
A product supplies the caption, the fuse and the axis; the sister product keeps a small `StagedSendHint` wrapper that observes its language setting and passes its caption, so its look and behaviour are unchanged, and its Messages scheme builds for the simulator.
`MotionRuler.swift` had stopped compiling for the sister product's iOS 16 target (`.transaction(value:)` is iOS 17); the ruler now drops every transaction's animation.

In uttt it stands over every screen (`UtttSendOverlay`, its own hosting controller above the screen host), so its fuse survives a screen swap.
It shows `UTM_SEND_HINT_MS` (3000, the sister product's number) after an insert is answered, in the compact drawer only; a send, a cancel, a new stage (the fuse restarts) or the drawer starting to grow hides it.
The caption is `uttt_say(UTTT_SAY_SEND_HINT)`, "Send".
Shots: `move_light_t1`/`move_dark_t1` (1.2s after the tap, no hint), `move_light_t4`/`move_dark_t4` (4.2s, hint), `inv_light_t4`, `inv_dark_t4`, `inv_dark_expanded` (none when expanded).

### An unanswered insert

Per `INSERT_GATING.md`, silence is the refusal.
Each insert arms a watchdog of `UTM_INSERT_SILENCE_MS` (500); what its silence means is `utm_insert_silence(attempt, compact)`: in compact, retry up to `UTM_INSERT_ATTEMPTS` (10, about 5s); expanded, keep listening and count nothing, because the host parks an accepted insert's answer there.
The stage generation still voids an overtaken loop, and a yes from any try (a late one included) stands every watchdog of that stage down.
Every firing is logged (`insert attempt N got no answer; retrying`).
After ten unanswered tries the drawer offers a pen-drawn door, "Send a board" (`UTTT_SAY_DOOR_SEND`, the Again door's `uttt_draw_door` pen), which re-inserts on a tap; it is the only part of the overlay that takes a touch.
The error path (an insert that fails with an error) is unchanged.
`touch dev.dropinsert` in the App Group (DEBUG only) swallows every insert unanswered, the only way to see this on a simulator: the log shows ten tries 0.52s apart, then the door (`door_dark`); removing the file and tapping it inserted at once and the hint followed (`door_tapped_hint`).
Tests: `uttt_msg_test` "insert:" rows and `ios-smoke`, each mutation-checked.

### The board as large as the sheet allows

Owner: the board as large as possible on every screen at every height, centred, with the words moved around it.
`uttt_sheet` no longer takes a measured box of words: the side is `min(width less the columns, height less the grab handle's 13pt margins and the bands)` and nothing else.
On the strip the words go in a column beside the ink (the verdict on the right over the rulebook, the waiting words and the spectator's line on the left) and wrap between words only, a single word too wide for the column scaling down instead; from half open they sit in the header band.
Every screen keeps its 38pt columns on the strip, or the waiting words had no room at all on a 340pt drawer on a 375 phone.
Tests: `uttt_anim_test` "every screen's board is as large as the sheet allows, compact and expanded" (seven sheets, four screens, the expected side worked out from the sheet's own numbers) and "its words sit beside it on the strip and above it in the band", plus `ios-smoke`; three mutations (words costing 20pt, the waiting words taking 60pt of height, the column overlapping the ink) went red on the named assertion.

| compact, 440x274 strip | before | after |
|---|---|---|
| live game | 268 (taller drawer) | 248, the height less the margins |
| end, winner / loser | 230 / 223 | 248 |
| spectator | 213 | 248 |
| waiting | 165 | 248 |

Expanded boards were already width-limited and are unchanged.
Shots: `before_*` and `after_*` for play, win, lose, watch and wait, compact and expanded; `before_sheet.png` and `after_sheet.png` put all ten side by side.

What is left:
- On a 375-wide phone the strip's word column is 32pt, so the waiting words and the verdict scale down toward half size there.
- The send door covers the lower third of the compact board while it is up.
- The hint and the end strip's verdict share the top-right corner when a finishing move is staged.

## 9. Auto-collapse on the render server, and the contact-sheet defects (2026-09-23, in progress)

Takes are in the session scratchpad `film2/ruler/takes/` (`before_light_1..5`, `before_dark_1..5`, `after_light_1`), with the scripts that made them (`film2/auto.sh`, `prep.sh`, `take.sh`).

### The auto-collapse, ported from foolish

- `shared/swift/MessagesKit/CollapseSlide.swift` is foolish's CollapseLayer idea, product-free: armed right before the app asks for compact, a drop of more than `UTTT_COLLAPSE_FLIP` is the flip, the sheet is laid out at the compact height from that frame (`uttt_drawer_rest`), the hosting view is pushed down by the travel left on a `CAKeyframeAnimation` (the host's critically damped spring, `uttt_collapse_push`, 120 keyframes over 600 ms), and each rider (`collapseRide`) takes its share back on a layer of its own.
- Riders: the header (the "you are" mark, the band's words, which fade on their layer, the ruler's red bar) take the whole push back; the board takes half and scales about its centre from `uttt_sheet` at every height; the doors take none.
- A rider takes no touches unless it asks (sheet-sized riders swallowed every tap on the board).
- didTransition comes about 50 ms BEFORE the compact height is handed, so the arm outlives it by 0.5 s.
- The drawer waits for the whole move (ink, highlighter, ring) and then `UTTT_MS_REST` (500 ms) before it collapses; the log showed the rest at ~570 ms.
- The paper is one fixed 1600-point sheet, bottom-anchored, so its grain never stretches as the drawer moves; the sheet's clip reaches up by the travel through a slide.

Measured (1 after take so far, light, relative to the drawer's top bar; per frame):

| mark | before (5 light takes): snaps / largest / rough | after (1 light take) |
|---|---|---|
| orange you-are mark | 0 / 0 / 24853 | 0 / 0 (rides the top within ~0.5pt per frame after a 4pt first-frame step, the icon's size lerp) |
| yellow headline | 0 / 0 / 24887 | 0 / 0 (fades on its layer) |
| rulebook door vs the green bottom bar | moved with the layout | constant to 1pt (bottom never moves: green 919 -> 912, the host's own 7pt) |
| magenta board centre | 4.2 / 110pt / 43599 | 3 / 48.7pt / 4442 - NOT FIXED |

**The board jump - found and fixed (2026-09-23, second pass).**
The cause was not the slide starting early: the board's content crawled on the main thread.
The slide's DEBUG probe (`CollapseSlide.probe`, every install, rider layout and each rider's layers per display frame, logged under the ruler) showed the board rider's layer transform exactly on the render server's curve, while the board's picture layer INSIDE the rider moved its model frame from the expanded rect to the compact one over ~400 ms in ~70 ms steps, with no CA animation on it.
Messages resizes the extension inside a UIKit animation block and the rider's nested hosting controller bridged that into an implicit SwiftUI animation of its content, rendered at the main thread's rate under a layer already scaled for the compact frame: 77 -> 29 -> 11 -> 4 -> 1pt off the drawer's centre.
Fix: a rider's root strips every inherited animation (`CollapseRiderHost.root`, `.transaction { $0.animation = nil }`); the sheet itself does the same for its own graph (`UtttDrawerSheet`, unconditional, replacing the `h`-scoped one that missed the flip's second pass).
Laying the nested host out synchronously inside `performWithoutAnimation` was tried first and changed nothing (measured), so it is not there.
Board centre against the drawer's centre, auto-collapse: largest one-frame jump 45.7pt (3 snaps) -> 0.7pt (0 snaps), 5 light + 5 dark takes.
The you-are mark then showed a 7pt first-frame step (the mark is 34pt compact and 46pt open and rode as one layer with its label); it and its label now ride apart, the mark scaling about its top left: 7.3 -> 1.3pt.

### The defects

1. The send hint hides in the frame it is sent or the drawer grows: the overlay's layer is hidden and committed at once (`hideHintNow`), and a drawer laid out taller than it rested counts as growing (a drag hands a height every frame; willTransition only comes at the release). SendHint takes `hidesAtOnce` (foolish keeps its fade).
2. `SendHintInk.white`: white arrow and caption ringed in the send blue; foolish keeps `.blue(outline:)`.
3. The first board of a process is painted synchronously at 1x (a ninth of the pixels) so the first frame has the lines, and sharpened off the main thread.
4. The words are set twice, in the column beside the ink and in the band, and the kernel shows each only where it fits (`SHEET_COLUMN_NEED`, `SHEET_BAND_NEED`), crossfading - no height squeezes them.
5. The headline and subline speak of the position one ply back until the kernel's frame says the ink has landed (`uti_say_before`).
6. The settlement half: a won block's big mark falls (780 ms) and then the win line draws (500 ms), at Send (channel B, `UTTT_CH_SETTLE`) and after the ink on an opened or arrived bubble; never at stage. `uttt_draw_settle` + `uti_draw_under` without them.
7. With a bubble staged the strip's right column starts under the hint (`SHEET_HINT_ROOM`).

Defects 1-7 are built and unit-tested (C tests mutation-checked: the push curve, the rest, the words crossfade, the hint room, the settlement order and composition), but not yet filmed.

## 10. The full drawer sweep, the C ruler reader, the win line (2026-09-23)

### The measuring tool is C now

`shared/tools/motion` (product-free): `motion_take.sh MOVIE OUT.tbl` pipes every composited frame from ffmpeg as raw RGB into `motion find`, which classifies every pixel by hue once and labels the whole ink map in one pass, and writes a fixed-layout text table (bars, clock, every square, quadrant-split when an ink repeats).
`motion score` scores takes against each mark's own anchor (the header the red bar, the doors the green bar, the board the drawer's centre; `--anchor` overrides), with ride.py's snaps/late/miss/rough and bars.py's jerk, host-spring floor (0.338 s), stray and travel; `--side FILE` scores off-centre board marks against the board's own scale (uttt writes that table with `make -C uttt/c build/uttt_side`).
It agrees with marks.py and ride.py to the digit on two takes (every column, every frame) and runs ~6x faster.
`make -C shared/tools/motion test`: 40 checks on synthetic frames and takes, each of 9 mutations (hue table, snap threshold, misses, late, quadrants, clock bits, side model, strip skip, area cap) went red on its named assertion.
marks.py, ride.py and marksplot.py are deleted; `shared/rig/lib/motionplot.py` charts the C table (y and x over time, offset from anchor) and holds no scoring.
The rest of `shared/rig/lib` (squares.py, bars.py, mse.py, window.sh...) still serves foolish's CollapseRuler pipeline and is not ported yet.
The palette and sizes are defined once in `shared/c/motion_ruler/motion_ruler.h` (module `CMotionRuler`, on SWIFT_INCLUDE_PATHS in both project.yml files); `MotionRuler.swift` has no colour literals left. foolish's own `CollapseRuler.swift` still carries its own palette.

### Per scenario (UtttRig, light unless named, normal speed, ruler on)

Largest one-frame jump of any mark against its anchor, pt; FAIL above 4.

| scenario | takes | board | corners | header | doors | verdict |
|---|---|---|---|---|---|---|
| (a) auto-collapse, before (e9d96ece) | 1 | 45.7 (centre) | 37.6 | 4.0 | 0.3 | FAIL |
| (a) auto-collapse light / dark | 5 / 5 | 1.3 / 1.4 | 2.3 | 1.3 | 0.7 | PASS |
| (b) the join move | 3 | 0.5 | 2.5 | 1.4 | 0.7 | PASS |
| (c) first-open drag 1.2s / flick | 3 / 3 | 7.2 / 47.7 | 7.2 / 56.5 | - | - | FAIL |
| (c) in-game drag 1.2s / flick | 3 / 3 | 40.0 / 183.5 | 44 / 211 | 2.7 / 32.7 | 80.5 / 366.7 | FAIL |
| (c) end drag 1.2s / flick | 3 / 3 | 34.9 / 113.9 | 38.8 / 113.8 | 1.7 / 4.4 | 69.7 / 228 | FAIL |
| (d) tap a bubble to open, before -> after | 3 -> 3 | 58.0 -> 0.0 | 58 -> 0 | 0 | 78.7 -> 0 | PASS |
| (e) arrival while open | 3 | 0 | 0 | 0 | 0 | PASS |
| (f) final move, block falls, win line | 3 | 1.9 | 2.8 | 1.3 | 0.4 | PASS (motion) |
| (g) Again, before -> after | 3 -> 3 | 207.2 -> 0.7 | 259 -> 2.7 | - | - | PASS |

Fixed in this pass:
- (d) Messages lays a new extension out at the whole window, then hands 840, a transient 293 and 840 within 5 ms; the drawer clock sprang through them and the board slid 58pt and back under a still drawer. A height handed while the sheet sat at the window's top is now taken at once (`UtttDrawerSheet`, `atWindowTop`).
- (g) The waiting and spectator boards had no collapse ride, so after Again the board sat at its compact place through the slide. One `boardRide` and one `wordsRide` (UtttDrawerSheet.swift) now serve all three screens.

Still failing (all three addressed in section 11):
- (c) Manual drags. The layout follows the drawer clock's spring, which the idb drag's ~40pt steps engage (the finger path follows at once only within 32pt): mid-drag the door sits up to ~100pt above the drawer's bottom and the board ~30pt off centre, then catches up in steps. The extension also committed only every ~70-100 ms during these takes (the machine was loaded). Needs a finger-drag rule that does not spring, and a device take.
- (f) At stage the drawer shows the whole settlement (the big mark and the win line) in one frame about 0.3 s after the move's ink, before Send; defect 6 says the settlement waits for Send. The still board drawn after the stage motion includes it.
- (d) A cold extension launch leaves the drawer blank for ~2-3 s on the simulator (Debug, log stream running); the first paint follows `load` by ~0.3 s.

Films, sheets and charts: session scratchpad `film3/` (`takes/`, `sheets/`, `charts/`, `scores/`, `score_table.png`, `fixes/`).

### The win line is twice the major grid line (owner)

`uttt_draw.c` `win_line`: 3x and 2.5x the major line's pen (`GRID_MAJOR_W`), was 2.7 and 2.3 (UI.html 08's note updated).
Measured on the ribbons (area over half perimeter): 2.09x the major line's ink width, was 1.27x; `uttt_anim_test` asserts at least 2x and goes red with the old widths.
Shots before and after, a won diagonal and a won row, staged bubble, sent bubble, expanded and compact: `film3/fixes/win_line_before_after.jpg`.

## 11. Drags, the win at Send, the cold launch, no pulse, the bubble (2026-09-23)

Takes, charts (motionplot.py), contact sheets and scores are in the session scratchpad `film4/` (`takes/`, `charts/`, `sheets/`, `scores/`, `table.md`, `bubble/`), with the scripts that made them (`sweep.sh`, `take.sh`, `st.sh`, `shot.sh`, `table.sh`, `cold.sh`, `bubble.sh`).

### The layout is the host's height, at once - the drawer spring is gone

The drawer spring (`uttt_drawer_*`, section 7) followed a height at once only within 32pt, and the simulator's drag injection steps 40-50pt, so mid-drag the layout sprang behind the finger.
The first fix kept the spring for jumps and decided "jump" by the host's word (willTransition arms a 500 ms window), never by a distance.
Measured, that was still wrong: at a drag's release Messages hands the final height once and animates the extension's view there itself, and a tap to expand does the same, so content laid out at the final height at once rides the host's animation.

| take | spring on jumps | at once |
|---|---|---|
| in-game slow drag and its release, board centre / door | 34.8 / 71.3pt | 0.7 / 1.1pt |
| tap to expand, board centre / door | 8.7 / 16.8pt | 0.5 / 0.3pt |

So every height is laid out as handed: `UtttDrawer`, `uti_drawer_*`, `UtttDrawerClock` and the window-top rest are deleted, and `CollapseSlide` loses `wouldFlip` and `isRunning`.
The auto-collapse keeps its slide (the one motion the host does not carry).
The window-top rest (section 10 (d)) is not needed without the spring: tap-to-open measured 0.0pt in both takes.
foolish does the same (`CollapseTween.step`: an unarmed height is `.follow`).

THE SIMULATOR IS THE LIMIT ON DRAG SMOOTHNESS: idb moves the drawer only every ~140 ms, in 40-50pt steps, with `--delta 4` as with the default.
A `sample` of the extension through a 2 s drag found its main thread idle (all samples in `mach_msg`), so the steps are the host's touch delivery, not our frame time; the layout follows each step in the frame it lands.
cliclick cannot drag the grabber (memory), so there is no smoother injection on this Mac; a device take is the real test.

### Per scenario, before (film3) -> after (film4)

Largest one-frame step of any mark against its anchor, pt; FAIL above 4.
Doors are the rulebook door against the green bar (the violet square is the pen stroke on the compact end screen, not a door).
(b) and (d) after are fresh takes (`r_*`): `rig.sh openbubble` finds foolish's felt bubble by colour and no longer finds uttt's, so the sweep taps the bubble directly.

| scenario | takes | board | corners | header | doors | after |
|---|---|---|---|---|---|---|
| (a) auto-collapse | 5 -> 2 | 0.7 -> 0.5 | 2.3 -> 2.5 | 1.3 -> 1.7 | 0.7 -> 0.4 | PASS |
| (b) the join move | 3 -> 2 | 0.5 -> 0.5 | 2.5 -> 2.7 | 1.4 -> 1.7 | 0.7 -> 0.6 | PASS |
| (c) first-open drag 1.2s | 3 -> 3 | 7.2 -> 0.7 | 7.2 -> 4.5 | - | - | corners 4.5 |
| (c) first-open flick | 3 -> 3 | 47.7 -> 0.7 | 56.5 -> 5.2 | - | - | corners 5.2 |
| (c) in-game drag 1.2s | 3 -> 3 | 40.0 -> 0.6 | 44.0 -> 5.0 | 2.7 -> 2.0 | 80.5 -> 1.0 | corners 5.0 |
| (c) in-game flick | 3 -> 3 | 183.5 -> 0.5 | 210.8 -> 3.7 | 4.0 -> 4.7 | 366.7 -> 1.3 | header 4.7 |
| (c) end drag 1.2s | 3 -> 3 | 34.9 -> 0.7 | 38.8 -> 4.5 | 1.7 -> 1.6 | 69.7 -> 0.7 | corners 4.5 |
| (c) end flick | 3 -> 3 | 113.9 -> 0.5 | 113.8 -> 4.7 | 4.4 -> 2.4 | 228.0 -> 1.0 | corners 4.7 |
| (x) tap to expand (new) | 0 -> 2 | 0.5 | 11.4 | 0.8 | 0.4 | corners 11.4 |
| (d) tap a bubble to open | 3 -> 2 | 0.0 -> 0.0 | 0.0 -> 0.0 | 0.0 -> 0.0 | 0.0 -> 0.0 | PASS |
| (e) arrival while open | 3 -> 2 | 0.0 -> 0.0 | 0.0 -> 0.0 | 0.0 -> 0.0 | 0.0 -> 0.0 | PASS |
| (f) final move, stage + Send | 3 -> 2 | 0.6 -> 0.7 | 2.8 -> 2.5 | 1.3 -> 1.7 | 0.4 -> 0.7 | PASS |
| (g) Again | 3 -> 2 | 0.7 -> 0.5 | 2.7 -> 2.7 | - | - | PASS |

What is left:
- The drag corners (4.5-5.2pt) are scored against the kernel's side for the bar-measured height; with the host stepping 40-50pt a frame, the residual jitters +-4pt about the model and does not grow through the drag (charts `c_*`). It needs a smooth (device) drag to tell a model offset from a real lag.
- Tap to expand: the board takes its expanded size in the first frame while the host grows the drawer over ~0.3 s, so the corners sit up to 11pt off the drawer's scale (centre 0.5pt). The fix is an expand slide on the render server, CollapseSlide's mirror; not built.

### The win shown before Send - fixed

The end of the game re-presents for its Again door ~0.3 s after the ink, through the still channel, whose plan drew the won block's big mark and the win line in one frame, before Send.
`UTTT_CH_DRAFT` (`uttt_anim.c`) is the stage's last frame at rest: ink and wash, the settlement held for Send; `showBoard` asks for it whenever the board it shows is an unsent draft.
Tests: `uttt_anim_test` "draft:" rows (the winning move, a block-taking move, a move that won nothing), each mutation-checked.
Filmed: `sheets/f_final_win_at_send.jpg` (the winning move: at stage the ink only, the Again door and the headline, no big mark or line; at Send the mark falls, then the line) and `sheets/k_block_at_send.jpg` (a block-taking move, move 80 of the diagonal fixture).

### Cold launch - the blank is the simulator's, not ours

`film4/takes/cold_menu_1`: a cold extension launched from the app strip, logged from spawn and sampled with `/usr/bin/sample UtttMessages -wait` at 1 ms.

| from process start | what | whose |
|---|---|---|
| 0 - 0.77 s | dyld in the simulator loading the Debug build's debug dylib (526 main-thread samples) | the simulator, Debug |
| 0.77 - 3.2 s | `UIApplication _accessibilityInit`: 1748 samples loading accessibility bundles (GeoServices, RealityKit, MapKit... .axbundle) | the simulator with idb's accessibility client on |
| 3.4 s | Messages connects to the extension | host |
| 3.82 s | `load` | ours: `MessagesViewController.init` ~25 ms, `viewDidLoad` ~12 ms |
| 3.82 - 4.54 s | `active`, `willBecomeActive`, the drawer's size handed | host |
| 4.54 - 4.77 s | paper 3 ms, the sheet, the first board at 1x (33 ms), first frame | ours, ~0.23 s |

Ours is ~0.25 s of ~4.8 s.
The accessibility init only runs because an automation client has accessibility on in the simulator; a device without VoiceOver does not load those bundles.
Nothing was changed; a Release take on a device is the measurement that matters.

### Owner decision: no pulse

The ring round the destination block after the highlighter lands is gone from every channel: `UtttMotion.pulse_at`, the frame's ring fields, the `UTTT_PULSE*`/`UTTT_MS_PULSE*` timings, the bridge mirrors and the Swift Canvas ring.
The highlighter still travels after the ink lands on the same timings, and a plan rests once the wash arrives (stage 600 ms, my replay 680, theirs and an arrival 760).
The ruler never read the ring: its squares are the board centre and corners, the header, the doors, the highlighter and the pen stroke.
Tests: the plan's `end_ms` on stage, theirs and arrival; a ring's tail put back (end + 300 + 2 x 620) went red on all three.

### The bubble: the board alone, the turn in the caption (owner)

The image of a game in play is the board alone, centred in the 300x195 frame (168pt, height-limited, 66pt clear of Messages' ~31x24pt badge); only a finished game keeps its words (the winner's mark and "wins", or "A draw", over "N moves").
`uttt_bubble(g)` owns it; the Swift snapshot carries the frame, so the off-main paint never reads the resident game.
The caption is `uttt_caption`: "O to play, top-left board", "X to play, anywhere", "New game?", "X won down the left in 21 moves", "Drawn in 21 moves"; a win whose line would not fit one row is "X won in N moves" (the diagonal fixture's 25-move win reads so).
One line, measured on UtttRig: a sent bubble is 309.7pt wide with 17pt padding each side (275.7pt for words) and captions set at 7.41-7.56pt a character ("Sent anywhere on the sheet" 196.7pt, "X won on the diagonal in 25 moves" 244.7pt), about 36 characters; `UTTT_CAPTION_MAX` is 32.
`uttt_msg_test` sweeps every caption the table can produce (every side, block, line, result and length 0-81): the longest is 32 characters.
ios-smoke: the in-play board centred, no words, its ink 3pt inside the frame on all four sides and none under the badge; the finished game's text column left of the board.
Mutations (not centred, words always, words never, the board under the badge, the old headline, the to-play mark, no fallback, a full stop, never the line) each went red on the named assertion.
Shots: `film4/bubble/before_after.jpg` (top: before, staged invitation, move and win; bottom: after, the invitation and win sent and the move staged, "O to play, top-left board").

## 12. Choppiness, memory, the two halves of a move, the rules sheet (2026-09-23, iPhone SE)

All of it on ONE simulator, `UtttSE` (iPhone SE 3rd generation, iOS 27, 375x667 points, 2x), `1AE07DDF-43F5-482C-85A0-F9C3A8CF7660`.
Films, contact sheets, profiles and benchmark tables are in the session scratchpad `film5/` (`bench/`, `prof/`, `takes/`, `sheet_*.jpg`), with the scripts that made them (`bench.sh`, `bench_open.sh`, `mem.sh`, `take.sh`, `reopen.sh`).

### The benchmark, one change at a time

`bench.sh`: the seeded board (`devgame 27`, seat a, seed 77), one tap staging an O, filmed at normal speed with the ruler on, five takes; `motion pace` (new in `shared/tools/motion`, C, test mutation-checked) counts the frames in which the cell's pixels really changed (past the codec's shimmer), the largest gap between them, the largest share of the ink one frame laid, and judder on a 60 Hz grid.
Opening a bubble is scored by the kernel clock's own log (`motion done N ms, K frames`), because the drawer's own slide moves every pixel of any box.

| step | change | commit | stage: content fps | largest gap | judder | open: frames in the plan |
|---|---|---|---|---|---|---|
| b0 | baseline (build before this session) | fa058e8d | 5.0-9.9 | 127-327 ms | 700-1030 | 5 in 711 ms |
| b1 | the App Group looked up once (dev files); the new motion plan | 2966f950 | 9.9-12.1 | 115-137 ms | 420-550 | - |
| b2 | the board's per-frame SwiftUI split into small observed views | d985b242 | 8.8-11.3 | 118-142 ms | 480-650 | 8 in 683 ms |
| b3 | the wash and the moving strokes on Core Animation layers set straight from the clock | 8c90b784 | 56.5-57.1 | 18-20 ms | 124-487 | 32-35 in ~690 ms |

What `sample` said at each step, heavy functions and lines:
- b0: of ~540 busy main-thread samples in a stage, 343 were `CA::Transaction::commit` -> `RB::SharedSurfaceGroup::render_updates`, 283 of them in `-[_MTLCommandBuffer waitUntilScheduled]` - SwiftUI's RenderBox re-rendering the board's two Canvases every display frame and then waiting on the simulator's Metal. The rest: the new position's board raster on the main thread (`UtttBoard.render`, `Uttt.fill` line 339 `fillPath`, about 40 ms, with `CGColorTransformConvertColor` under it; a CG benchmark put the colour conversion at only 10-15% of the fill, so it was left), the ink Canvas's `Path` building (37), and on an open 30 samples in `UtttRuler.on` -> `containerURLForSecurityApplicationGroupIdentifier` inside a view body.
- b2: splitting the SwiftUI views changed nothing (RenderBox 523 of ~900) - any Canvas render is the cost on this simulator, however small.
- b3: nothing that moves is SwiftUI any more (`UtttMotionView`): the wash is a layer's colour and frame, the ink and the outline are bitmaps of only their own polygons (Core Graphics, well under a millisecond) handed to the render server. The display link now ticks every frame.

Drag: an earlier `sample` of a drawer drag found the main thread idle (section 11); not re-measured.

### Memory

Floor: `dev.empty` (DEBUG) makes the extension show nothing - 20.9 MB footprint (21.9 peak) on this simulator, Debug.

| state | before | after |
|---|---|---|
| idle compact, seeded board | 42.5-43 MB (peak 44-46) | 31-32 MB (peak 32.5) |
| peak, a finished board staged (bubble baked) | not measured | 40.8 MB (peak 45.4) |

The cut: 689 untagged 64K regions (10.8 MB) that the empty extension does not have were RenderBox's Metal shared buffers from SwiftUI Canvases; the pen drawings that never move (the "you are" mark, the rulebook door, Again) are now painted once into images (`UtttInkImage`), and untagged memory went to 16K.
**Not yet at the target (floor + 5 idle, + 10 peak).** Left, measured against the floor: `__DATA` +3 MB (the bridge's static display list, `MAX_PT` 260,000 points plus a duplicate `first/n/rgba` copy of the polygons), Malloc Small +3.5 MB (the Swift copies of the whole display list the off-main raster and the bubble paint take), CoreAnimation +1.8 MB, CG raster 1.1 MB (the board image).

### The two halves of a move (owner)

- PRE, at stage: the small mark, then the won block's big mark, then the win line, one after another; the highlighter stays on the block the move was played in; the block the move sends the other player to is outlined by the pen in the highlighter's own rect and colour (`uttt_wash_rect`, `uttt_wash_rgba(1)`), drawn round once everything else has landed. Freed ("anywhere"): the outline is the whole sheet, the tint's own "anywhere" rect. Game over: no outline.
- POST, at Send: only the highlighter moves, to the outlined block, and the outline fades as it lands. It plays on the board already up (`UtttModel.sent`), no screen rebuild.
- The receiver: small mark, big mark, line, then the highlighter; no outline.
- `uttt_motion` in `uttt_anim.c` owns all of it; `uttt_draw_outline` in `uttt_draw.c`. Tests in `uttt_anim_test.c`, 8 mutations each red on the named assertion.
- Change of mind: one step - the old draft (and its big mark) gone at once, the new one drawn in with its own settlement and outline; nothing un-draws, the highlighter never moves.
- A tap that is not a move does nothing: `utm_can_replace` (kernel) is the only question a tap on a board with a draft asks; exhaustively tested against the legal list, 3 mutations red; `ios-smoke` covers the bridge.
- My own bubble, just sent, opens quiet (the settled board) - `present` compares with `sent`.
- The rules are a sheet of their own (`rulebook`, SwiftUI `.sheet`, as foolish's `GameSurface` does); a swipe down closes the rules and leaves the game up (`sheet_rules_dismiss.jpg`).

Sheets: `sheet_board_win_stage.jpg`, `sheet_game_win_stage.jpg`, `sheet_normal_stage.jpg`, `sheet_normal_send.jpg`, `sheet_change_of_mind.jpg`, `sheet_invalid_tap.jpg`, `sheet_receiver_open.jpg`, `sheet_receiver_board_win.jpg`, `sheet_rules_dismiss.jpg`, `sheet_send_post.jpg`.

### Open

- ~~At Send the drawer is seen replaying.~~ Closed in section 13.
- Memory above target (section 13 has the next steps).
- ~~The partial ink frame visible in the first frame after Send~~ - the same cause, closed in section 13.
- On the SE the strip's word column is narrow, so "Your move / Anywhere you like" and "You win / Diagonal" set small beside the board (seen in `sheet_game_win_stage.jpg`).

## 13. My own move replaying after Send, and memory (2026-09-23, iPhone SE)

Films, sheets and scripts are in the session scratchpad `film6/` (`take.sh`, `open.sh`, `prep.sh`, `clock.py`, `wash.py`, `sheet_send_before2.jpg`, `sheet_send_after1.jpg`).

### The replay was live, and it was our own bubble coming back

The hypothesis was that Messages showed a stale snapshot of the extension after Send.
The ruler's clock says otherwise: `clock.py` reads the 14 cells in every composited frame, and through the replay the clock advances 16-17 ms a frame with no repeat and no step back, so every replayed frame was this process drawing live.
The log gave the cause: in a drawer opened by TAPPING a bubble (bound to its session, the way the owner plays), pressing the arrow delivers the sender's own bubble through `didReceive` about a second BEFORE `didStartSending` (`select own bubble`, `receive` at +198.648 s; `send` at +199.684 s).
`didReceive` treated it as an arrival: a new screen with the whole move played again (channel 4), and then the Send's own post-settlement (channel 6) played on top of that.
A drawer opened through + is unbound and gets no echo, which is why the earlier scripted takes could not show it.

The fix is foolish's own rule (`StagedBubbleRouting.isMine`, round 12 #11): a bubble this device staged or sent coming back is not an arrival and is dropped (`receive-dropped`).
The echo is the send landing, so the post-settlement plays then; `settleSent` plays it once per bubble, whichever of the echo and `didStartSending` comes first, and hides the send hint in that frame.

Proof (`take.sh`: tap a bubble, play a move, let the drawer collapse, press Send, 9 s at normal speed; `wash.py` tracks the highlighter's centroid and area in every compact frame and counts every frame after it first arrives where it has left again):

| take | before (74d53f53^) | after (74d53f53) |
|---|---|---|
| 1 | 17 replay frames, from 0.5 s after landing | 0 |
| 2 | - | 0 |
| 3 | - | 0 |
| 4 | - | 0 |
| 5 | - | 0 |

### Memory, one change at a time

`film5/mem.sh`, idle compact on the seeded board, ruler off, 3 opens each; floor (`dev.empty`) 21 MB.
Stage fps is `film5/bench.sh`, 5 takes, after the last step.

| step | change | commit | idle footprint (peak) | what moved (vmmap dirty) |
|---|---|---|---|---|
| m0 | before | 74d53f53 | 31-32 MB (32.4) | UtttKit `__DATA` 1168K, CG raster 1200K, CoreAnimation 1968K, paper `Data` 689K |
| m1 | display list on the heap, a whole board handed over (`uti_take`), no duplicate first/n/rgba, no Swift copy | 07ee192a | 30 MB (31.4) | UtttKit `__DATA` 1168K -> 32K |
| m2 | board and paper one IOSurface each (`UtttBitmap`), paper written as BGRA by the kernel | 0e995c37 | 27 MB (28.0) | CG raster 1200K -> 112K, CoreAnimation 1968K -> 160K, paper `Data` gone; IOSurface 1856K |

Stage benchmark after m2: 56.0-56.8 fps, largest gap 18-20 ms (b3 was 56.5-57.1, 18-20).
Peak across a stage (the bubble baked and inserted): 41.5-41.7 MB.

Caveats and what is left:
- The simulator's `footprint` does not charge the two IOSurfaces (1.8 MB) to the extension; a device will. Counted, the idle drawer is about 28.8 MB, 7.9 over the floor: the target (floor + 5) is not met yet.
- The rest of the idle gap is not ours to allocate: `__DATA` of system frameworks SwiftUI touches (+2 MB against the floor) and SwiftUI's own heap (metadata, attribute graph, +3.5 MB of Malloc Small). Getting under floor + 5 means less SwiftUI in the drawer, not smaller buffers.
- The stage peak (+20 over the floor, target + 10) is the bubble: a 900x585 bitmap at 3x (2.1 MB), its own paper, the board's polygons, and Messages' encode of the image on insert. Next: bake at 2x, paint the bubble's paper as BGRA straight into the renderer, and measure the insert with `heap` during the stage.


## 14. The bubble's memory, no SwiftUI, the expand slide, the outline (2026-09-23, iPhone SE)

Films, contact sheets, charts and scripts are in the session scratchpad `film7/` (`peak.sh`, `stagelog.sh`, `ref.sh`, `take.sh`, `st.sh`, `regress.sh`, `sc.sh`, `fillgreen.py`, `xsheet.py`, `takes/`, `charts/`, `outline/`).
Stage fps is `film5/bench.sh` (5 takes unless noted); memory is `film7/peak.sh` (3 opens: idle compact on the seeded board, then the peak across one stage with the bubble baked and inserted).
Floor (`dev.empty`): 21 MB.

| step | commit | stage fps | largest gap | idle MB | stage peak MB |
|---|---|---|---|---|---|
| s0 before this session | 839e2105 | 56.5-57.1 | 18-20 ms | 27 | 40.2-41.4 |
| s1 the bubble at 2x, one 8-bit opaque context | 0da7d7a6 | 56.5-57.1 | 18-20 ms | 27 | 32.7-32.9 |
| s2 no SwiftUI in the extension | be981b68 | 57.1-57.5 (3 takes) | 18-20 ms | 22-23 | 26.5-26.6 |
| s3 the expand slide | 763c5d46 | 56.5-58.3 | 18-22 ms | 22 (one open 25) | 26.5 (one 29.6) |
| s4 the rougher outline | 8b8b64eb | 56.5-57.5 | 18-20 ms | 22 | 26.5-26.6 |

### s1: the stage peak was the bubble's paint

`UtttLog.mem` (DEBUG) logs the footprint and its peak at each point of a stage (`film7/stagelog.sh`).
Before: stage 28.8, painted 30.8 with the peak at 42.8, inserted 32.7.
`UIGraphicsImageRenderer` chose an extended-range format and the paper went through a `UIImage`; the paint alone was 12 MB over idle.
Now one `CGContext`, sRGB, 8 bits a channel, no alpha (`noneSkipFirst`), handed over by `makeImage` copy-on-write: painted peak 30.0, and the paint takes 25 ms instead of 90.
2x against 3x on the SE's transcript: the 300-point bubble is shown at 252 points (504 pixels), and the two sent bubbles differ by a mean 2.4/255 (the grain's resampling), so the bake is 2x (`film7/bubble_2x_vs_3x.png`).
Open question for a device: a 3x phone shows the bubble at up to ~300 points, 900 pixels, so a 2x bake is upscaled 1.5x there.
What is left of the peak is the new position's board raster during the ink (+1.5 MB, the old surface still on screen) and Messages' encode at insert (+1 MB, gone a second later).

### s2: the extension has no SwiftUI

Every screen is a `UtttSheetView` (UIKit): the paper layer, the content masked to the sheet, one kernel layout (`uttt_sheet`) per height set as frames inside `performWithoutAnimation`, and each rider registered with `CollapseSlide`.
Words are `UILabel`s (wrapped at spaces in the column, one line in the band, scaled to half at most), the doors and marks are layers of the kernel's polygons (`UtttInkView`), the board is `UtttBoardView` (one `UITapGestureRecognizer` asking `Uttt.hit`, 81 `UIAccessibilityElement`s from `uttt_cell_rect` and `uttt_say_cell`), the rules are a page sheet `UIViewController` (a swipe down closes the rules only), and the DEBUG seat picker is UIKit.
`UtttModel` and `UtttMotionClock` are plain classes with callbacks; Combine is gone too.
Neither the extension binary nor UtttKit links SwiftUI or Combine (otool, Release); only the UtttPreview harness keeps SwiftUI.
shared/: `CollapseSlide` lost its SwiftUI adapter (nothing used it) and publishes its run to observers; `MotionRuler` is layers and a display-link clock; `SendHintView` is the send hint on Core Animation (the bob a repeating keyframe animation, the ink one bitmap); `SendHintMetrics` holds the numbers foolish's SwiftUI `SendHint` now reads too. foolish builds.
One regression found and fixed on the way: the board was bound before the model restarted its clock, and one frame showed the new move's whole mark before the ink began (bench: largest gap 45 ms, 52-55 fps); the board is now bound in the layout pass.
Screens before and after, light and dark: `film7/before_light_sheet.jpg`, `film7/sheet_uikit_light.jpg`, `film7/sheet_uikit_dark.jpg`; the pixel difference against the SwiftUI screens is 0.3-1.1 of 255 on the game screens (the "you are" label is two labels 2.8 points closer, as the VStack was).

### s3: the expand rides the host's spring

A tap to expand hands the tall height once and the host animates the view's bounds itself: read off the layer, a `CASpringAnimation` on `bounds.size` (and `position`), additive, mass 1, stiffness 333.3, damping 36.5 (critically damped), no initial velocity, 0.506 s, from 387 points shorter.
Content laid out at the tall height sat anchored to the drawer's top while the drawer was still short.
Now `UtttSheetView` reads that animation and `CollapseSlide.grew` carries every rider along the layout for the drawer's height at each moment (`uttt_spring_left`, C, beside `uttt_collapse_push`: any damping and an initial velocity), begun by the host's own commit; nothing is pushed.
The doors and the ruler's green bar ride the drawer's bottom (`dy = s`, which is also "none of it" for a collapse).
On the SE the drawer's bottom bar runs below the screen through an unridden expand, so the before takes are scored with it filled in (`fillgreen.py`, linear between the frames that see it).

| tap to expand (SE, `motion score --whole`, largest one-frame step) | takes | board centre | corners | header | door |
|---|---|---|---|---|---|
| before (763c5d46 with the ride off) | 3 | 33.2 | 24.9 | 2.5 | 6.4 |
| after | 4 | 1.0 | 1.8 | 1.5 | 1.0 |

Charts: `film7/charts/x_before_1.png`, `x_after_1.png`; contact sheet `film7/sheet_tap_expand_before_after.jpg`.
The rest, after, 2 takes each (largest one-frame step, pt): auto-collapse board 0.8, corners 3.5, header 1.5, doors 1.0; slow drag 1.1 / 3.2 / 1.5 / 1.0; flick 1.0 / 2.5 / 1.5 / 1.7; tap a bubble to open 0.1 / 0.0 / 0.0 / 0.0; arrival 0.0 / 0.0 / - / 0.0; Again 1.0 / 3.5. All pass (4 pt).

### s4: the outline is a rough.js rectangle (owner: "not rough enough")

`uttt_draw_outline` took the long grid lines' length falloff (offset .0074 on a block) and a thinned pen; now rough.js's defaults at the pen's 1.5 (what a mark gets), whose end jitter crosses the sides at the corners, and the marks' pen.
Tests (`uttt_anim_test`): the top side strays more than the tamed worst over five seeds, every box has a side past a corner, the bounds are the tint's rect within a hand's overshoot, the same seed draws the same wobble; four mutations each red on the named assertion.
An explicit seeded corner overshoot was tried first and dropped: rough.js's own end jitter at this offset already crosses the corners, and a mutation removing the extra overshoot changed no measurement.
Shots: `film7/outline/outline_before_after.jpg` (SE, light and dark).

## 15. Release-candidate regression pass (2026-09-23, iPhone SE and Pro Max)

Two simulators, one booted at a time: `UtttSE` (375x667, 2x) and `UtttRig` (440x956, 3x).
Screens, contact sheets and scripts are in the session scratchpad `film8/` (`sheet_se_release.jpg`, `sheet_se_debug_flow.jpg`, `sheet_se_debug_win.jpg`, `sheet_pm_release.jpg`, `sheet_pm_debug.jpg`; `bench_pm.sh`, `peak_pm.sh`, `reopen_pm.sh`, `cell.sh`, `sheet.py`).

### The bubble at the sender's own scale

`uttt_bubble_scale` (C) takes the display scale and clamps it to 2..3; the snapshot reads `traitCollection.displayScale` on the main thread and the off-main paint bakes at it (cb2cdeac).
The context is unchanged: sRGB, 8 bits a channel, no alpha.

### Numbers

Stage fps is `film5/bench.sh` (SE) and `film8/bench_pm.sh` (Pro Max, the same seeded board and a cell of the top-left block), 3 takes; memory is `film7/peak.sh` / `film8/peak_pm.sh`, 3 opens (idle compact on the seeded board, then the peak across one stage with the bubble baked and inserted).

| size | bake | stage fps | largest gap | idle MB (peak) | stage peak MB |
|---|---|---|---|---|---|
| SE, 2x | 2x | 56.5-57.1 | 18 ms | 22 (23.3-23.4) | 26.4-26.5 |
| Pro Max, 3x | 3x | 57.4-63.8 | 18 ms | 23-25 (24.4-25.3) | 34.5-36.0 |

The SE's stage peak is where it was (26.5, section 14), well under 31.
The Pro Max figures are the first on that size; there is no 2x-bake baseline there to subtract.
Pace counts over 60 fps are the counter's rounding over a 157 ms span, not frames the display does not have.

### Release build on the simulator

UtttMessagesApp Release, simulator slice, installed on both sizes.
On both: + opens the drawer and the invitation auto-stages (`insert attempt 1` then `inserted` 150-175 ms later), the send hint appears after 3 s, Send lands the bubble, tapping the invitation opens the creator's own "Waiting" screen, a drag collapses and expands, dark mode leaves the paper light and the hint legible.
The rules sheet cannot be reached in Release on one simulator (one participant cannot join its own invitation); the path is not `#if DEBUG` and was checked in the DEBUG pass.
Release binaries: no `dev.` strings, no em dashes, no SwiftUI or Combine linked (`otool -L`, the app, the extension and UtttKit).

### DEBUG two-seat flows, both sizes

All as settled (sections 12-14): join, a normal move (pre: the mark and the outline, the tint stays; post: only the tint moves), change of mind (the old draft gone at once, nothing un-draws), illegal taps (no log line, no pixel change but the hint's bob), a block-taking move (small mark, then the big mark, at stage), the winning move (the line at stage, "You win" / "X wins", the Again door, the caption "X won in 25 moves" on one line), an arrival while open, my own bubble tapped after Send (nothing plays), Again stages a new invitation.

### Defects found and fixed

- The Again door started at x 0 while the rulebook kept the 13-point margin: on the SE it ran to the screen edge, and on a rounded display under the corner. `uttt_sheet` now owns both door rects; a test holds both inside the margins on every sheet (fafb4396).
- The send hint's bob crest ran 11.6 points above the drawer, which Messages clips, so the arrowhead was cut flat at every bob on both sizes. The hint's container now starts at `UTTT_SHEET_HINT_TOP` (28, was 14), so the crest's ink is where the rest used to be; tests hold the crest inside the drawer and the verdict column under the hint (85f1157f).

### Open

- From a drawer opened with + (unbound), the send hint stays up for up to a second after Send, until `didStartSending` arrives; a tapped (bound) drawer hides it at the echo. Seen on both sizes (`34_win_send_mid`).
- On the SE, a drawer opened by tapping a bubble is 375x647, within 40 points of the window, so `viewDidAppear` reads it as window-sized and the 3 s "ready" deadline logs a fault on every such open. Only an invitation's insert waits on it; nothing visible breaks.
- One Pro Max process that opened the finished game expanded, then the rules, then Again, logged a lifetime peak of 47.4 MB (the SE's equivalent 31.1); an in-play expanded open plus the rules peaks at 24.6. Not attributed.
- The strip's word column is narrow on both sizes: "Nobody / has / taken it / yet" and "You / win" wrap a word a line (section 12's open item).
- The rules sheet's "Back" is a plain text button, not a pen-drawn door.

## 16. Closing the release-candidate items (2026-09-23/24, iPhone SE and Pro Max)

Screens, logs and scripts are in the session scratchpad `film9/` (the items) and `film10/` (the first motion take).

### The send hint after Send from a + drawer (not fixed - no earlier signal exists)

Probed on the SE with every notification the extension process receives (a nil-name observer) and a per-frame poll of the selection, the active conversation, the view and window sizes, key-window state and the presentation style.
From a + drawer the human never touched (the Release path: + opens it and the invitation auto-stages), NOTHING reaches the process between the tap on Send and `didStartSending`, about 1.1 s later on the simulator; `willResignActive`, the drawer's dismissal and the keyboard all come after it.
When the human HAS touched the drawer first (the DEBUG seat picker), a private keyboard-focus notification (`_UISceneDidResignTargetOrAncestorOfKeyboardEventDeferringEnvironmentNotification`) fires about 0.17 s after the tap, but it also fires on a tap into the compose field and never on the Release path, so it is not used.
foolish has no earlier signal either (its hint follows the same send state).
The hint stays hidden at `didStartSending`, the earliest reliable signal; a bound (tapped) drawer still hides it at the echo. Worth re-measuring on a device, where the host's send latency may differ.

### Done

- The ready test is the kernel's (`utm_drawer_up`: window height, view height, expanded): a view as tall as its window never counts (the + drawer's first appear, b270e078), an expanded drawer short of it always does (the SE's tapped drawer, 647 of 667), a compact one must be 40 points short. SE log: `appear 375x667 expanded` rejected, `375x647 expanded` counted, no ready fault. Tests in `uttt_msg_test` and `ios-smoke`, three mutations red.
- The rules sheet has no Back (owner): a swipe down closes the rules only (checked on both sizes), and VoiceOver's escape closes it too.
- The strip's word column: a width-limited board leaves 40 points beside it on the SE (375x260) and Pro Max (440x343) strips and no band above or below it, so the column carries the headline alone; its second line fades in past 60 points (`uttt_sheet` `sub_alpha`). "Waiting" now stands alone on the SE strip instead of "Nobody / has / taken it / yet"; the band carries both lines once open. "You / win" stays two lines (a two-word stamp). Four mutations red.
- "Copy code" on the end screen (owner): `uttt_replay_url` writes `https://www.foolish.cards/uttt/<base32 of uttt_encode>` and `uttt_replay_read` reads it back (lower case and a trailing query too); `uttt_test` round-trips every finished test game through the URL, four mutations red (including the address spelled out, so a typo in the macro fails). `uttt_sheet` places the door (`copy`) between Again and the rulebook at their height, expanded only (the strip's 46-point column has no room for words and the board is never shrunk); it reads "Copied" once the link is on the pasteboard. Checked on the Pro Max: the pasteboard held `https://www.foolish.cards/uttt/NSA7JGY2RPATFLZNH2ETBDNTUICA`.
- **The `/uttt/[code]` web route does not exist yet**: it must be built before the copied link resolves. Since section 17 the code carries the drawing seed too.

### Memory

The 47.4 MB Pro Max peak did not reproduce.
The same sequence (the finished game opened expanded by tapping it, the rules, a swipe down, Again) peaks at 25.5 MB on the SE and 28.5 MB on the Pro Max in a fresh process; with the first drawer dragged open, the rules opened there, collapsed, sent, and the bubble tapped within 1.5 s so ONE process hosts both drawers (the 47.4 run's shape), 32.4 MB on the Pro Max, the extra 4 MB from the 3x board repainted as the drag hands new heights.
Both are inside floor + 10.

### Open

- ~~The owner's flick-collapse jumps~~ - found and fixed in section 17. One simulator take (`film10/takes/pm_end_fcol_1`, the end screen): a hard flick takes the drawer past compact to the minimised grab bar; the board's centre rides the drawer's centre to 0.3 pt, and the corners step 10-15 pt a frame only because the board's side follows the host's height (32-68 pt a frame). The simulator does not show the jumps yet; the full sweep (every gesture, every screen, both sizes, width and height as metrics in the C tool) and a device capture tool are still to do.

## 17. Every drawer transition, measured, and the flick collapse fixed (2026-09-24, Pro Max and SE)

Takes, charts, scores, tables and scripts are in the session scratchpad `film10/` (`takes/`, `charts/` one per take, `scores/`, `table_pm_final.md`, `table_se.md`, `worst_takes.jpg`, `sweep.sh`, `st.sh`, `table.sh`, `strip.py`, `dev/` the owner's video through `motion grid`).

### What the owner saw, and why the simulator never showed it

A FLICK lets go mid-drawer: Messages hands the extension the final height ONCE and animates the view's bounds there itself, a `CASpringAnimation` on `bounds.size`, additive (k 333, c 29, the finger's velocity).
The sheet laid out at the handed height at once, and nothing rode that animation: in the release frame the board shrank to its compact size at the card's top, Again vanished, the doors jumped to mid-drawer, and blank paper filled the card below while it slid down.
The expand already rode the host's spring (section 14, `rideHostGrowth`); a release downward did not.
The earlier simulator "flick" (idb 72 to 720 in 0.15 s) dragged the drawer the whole way by finger, so there was no release left to animate; a short fast flick (0.1 s, 185 points) reproduces the device exactly (`probe_fcol`, `worst_takes.jpg` top row).

The owner's 1.0(5) recording, measured with `motion grid` (no ruler in a TestFlight build: the drawer's top and the board's four heavy grid lines):

| owner's video | board centre vs the drawer's middle | board side |
|---|---|---|
| expanded, at rest | -7.9 pt | 361.6 |
| flick collapse, the release frame (3.783 s) | -229.5 pt, a 221.6 pt step | 261.2, a 100.3 pt step |
| a finger drag down (19.29-19.44 s) | -7.9 to -9.9 pt, at most 0.5 pt a frame | 361.4 to 343.8 (width-limited, then shrinking) |
| its release frame (19.455 s) | -104.2 pt, a 94.3 pt step | 256.7, an 87.1 pt step |

So on the device a finger drag re-lays out every frame and follows the drawer; the jump is the release.

### The fixes

- `CollapseSlide.follow` (was `grew`): the host's own animation either way. `UtttSheetView.rideHost` reads the spring off the layer for a taller OR a shorter height, and every rider follows the drawer's edges on the render server (`uti_spring_left`); the clip reaches down through a followed shrink. The run carries `pushes` (only the auto-collapse pushes) and `shrinks`.
- A re-handed height does not end a followed ride: the host hands 289.0 and then 289.00000000000006 20 ms into a release, and "a new height ends the run" snapped every rider (a hard flick to the minimised drawer, corners and doors 10.4 pt). A change must be over 1 pt.
- `placeDoor` (one owner for Again and Copy code): both were hidden in the first frame of any collapse ride (`door_alpha` 0 at the compact layout) and popped in on an expand; now they fade on their ride with the kernel's `door_alpha` at the drawer's height.
- The spectator screen had no ride for its words and no `at` (it did not build after the door change); it now lays out like the others.
- `uttt_sheet`: a taller drawer never gives a smaller board, at ANY height. The bands grow 2 x 72 points on the smoothstep over 360-530, 1.27 points of band per point of drawer at the steepest, so on a 430 or 440 wide phone a height-limited board SHRANK 4 points while the drawer grew through 462-485 - a size reversal in every drag through it. The limit is now the largest that never falls and never passes the raw one. The test checked monotony only on the strip; it now sweeps every height and the 430 width; the clamp removed goes red.

- The replay link carries the drawing seed: the code is `[0..3]` the seed, int32 big-endian (the wire message's seed), then `uttt_encode`'s moves, so a web replay draws the same napkin. `uttt_replay_url(g, seed, ...)`, `uttt_replay_read(url, g, &seed)`; `uttt_test` round-trips 1000 games with seeds across the int32 range; a seed not written or read in the wrong byte order goes red.

### The measuring tool (shared/tools/motion)

- `mt_board`: the board's width and height from its four corner squares, per frame; the largest one-frame step, reversals beside the drawer's own (1 pt hysteresis), and with `--side` the residual against the kernel's side for the drawer's height. `motionplot.py` charts width, height and the drawer's height in a fourth panel.
- `off`: frames a mark sits outside the drawer.
- `--bottom first[:HC]`: THE PAINTED GREEN BAR IS CONTENT. Content that jumps takes its bar with it and scores as riding it: the owner's bug scored 0.7 pt against the painted bar and 160 pt against the drawer's resting bottom. Past compact (`HC`, the compact bars' distance) the drawer is a sliding card. The resting bottom has its own blind spot: the host moves its whole card 6.7 points when the style changes (913 compact, 919.67 expanded, the card's top keeping its place over the content), so every table below gives both.
- `motion grid` / `motion_grid.sh`: a recording with no ruler.
- 33 new checks (75 in all); each new assertion mutation-checked (12 mutations, each red on its named assertion; three that first failed to compile were rewritten to compile).

### Per scenario, Pro Max (UtttRig, light), before (97f38e70) -> after

Largest one-frame step of any mark in the group against its anchor, pt, over 3 takes each; the first four columns against the painted bars, then the board centre and the doors against the drawer's resting bottom (the column that sees the owner's jump; it includes the host's 6.7 pt style shift).
Gestures: `dexp`/`dcol` drag 1.2 s, `fexp`/`fcol` flick (0.1 s, released mid-drawer), `hcol` hard flick to the minimised drawer, `auto` the auto-collapse (a move; the final move on the end screen; Again for first-open).
Thresholds: centre 1 pt, corners, header and doors 4 pt, 0 off, no size reversal the drawer did not make.

| scenario | takes | board centre | corners | header | doors | centre / doors vs resting bottom | off | size step | size vs side | extra size reversals | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|
| first dexp | 3 -> 3 | 0.7 -> 0.7 | 4.7 -> 4.3 | - -> - | - -> - | 4.3 / - -> 4.3 / - | 0 -> 0 | 16.0 -> 8.7 | 8.7 -> 8.0 | 2 -> 2 | FAIL corners,reversal |
| game dexp | 3 -> 3 | 0.7 -> 0.7 | 4.3 -> 4.3 | 1.3 -> 1.7 | 1.3 -> 1.0 | 3.0 / 6.7 -> 3.0 / 6.3 | 0 -> 0 | 8.3 -> 8.7 | 8.7 -> 8.3 | 3 -> 0 | FAIL corners |
| end dexp | 3 -> 3 | 0.7 -> 0.7 | 4.2 -> 4.3 | 1.3 -> 1.3 | 1.4 -> 1.0 | 3.0 / 6.7 -> 3.0 / 6.3 | 0 -> 0 | 12.7 -> 14.7 | 8.0 -> 8.3 | 11 -> 0 | FAIL corners |
| first dcol | 3 -> 3 | 1.0 -> 0.8 | 1.9 -> 1.6 | - -> - | - -> - | 3.5 / - -> 3.5 / - | 0 -> 0 | 14.3 -> 14.3 | 2.0 -> 1.7 | 8 -> 0 | PASS |
| game dcol | 3 -> 3 | 0.8 -> 0.8 | 1.6 -> 1.6 | 0.7 -> 0.7 | 1.7 -> 1.7 | 3.5 / 6.7 -> 3.2 / 6.7 | 0 -> 0 | 8.3 -> 8.6 | 2.2 -> 1.7 | 12 -> 0 | PASS |
| end dcol | 3 -> 3 | 1.5 -> 1.5 | 2.0 -> 2.0 | 0.7 -> 0.7 | 2.0 -> 2.0 | 1.2 / 2.7 -> 1.2 / 2.7 | 0 -> 0 | 14.3 -> 8.3 | 2.2 -> 1.7 | 12 -> 0 | FAIL centre |
| first fcol | 3 -> 3 | 0.7 -> 0.8 | 1.0 -> 2.8 | - -> - | - -> - | 132.3 / - -> 2.0 / - | 0 -> 0 | 102.7 -> 30.7 | 1.7 -> 5.7 | 0 -> 0 | PASS |
| game fcol | 3 -> 3 | 0.7 -> 0.5 | 1.0 -> 2.8 | 3.3 -> 1.3 | 0.7 -> 0.7 | 117.7 / 235.7 -> 1.7 / 4.0 | 0 -> 0 | 107.0 -> 29.3 | 1.7 -> 5.0 | 0 -> 0 | PASS |
| end fcol | 3 -> 3 | 0.7 -> 0.5 | 1.0 -> 3.0 | 3.3 -> 1.7 | 0.7 -> 0.7 | 159.8 / 319.0 -> 1.8 / 3.3 | 0 -> 0 | 107.0 -> 30.7 | 1.7 -> 5.7 | 0 -> 0 | PASS |
| first auto | 3 -> 3 | 0.7 -> 0.7 | 2.7 -> 3.0 | - -> - | - -> - | 1.3 / - -> 1.7 / - | 0 -> 0 | 28.7 -> 22.7 | 5.3 -> 5.3 | 0 -> 0 | PASS |
| game auto | 3 -> 3 | 0.7 -> 0.7 | 3.0 -> 2.7 | 1.7 -> 1.3 | 0.7 -> 0.7 | 1.7 / 2.7 -> 1.7 / 3.0 | 0 -> 0 | 27.2 -> 26.0 | 5.3 -> 5.3 | 0 -> 0 | PASS |
| end auto | 3 -> 3 | 0.7 -> 0.7 | 3.0 -> 3.0 | 1.7 -> 1.7 | 0.7 -> 0.7 | 2.0 / 3.3 -> 2.0 / 3.3 | 0 -> 0 | 26.7 -> 25.0 | 5.7 -> 5.7 | 0 -> 0 | PASS |
| first fexp | 3 -> 3 | 0.8 -> 1.2 | 3.0 -> 3.2 | - -> - | - -> - | 26.7 / - -> 26.7 / - | 0 -> 0 | 29.0 -> 29.7 | 7.4 -> 7.7 | 6 -> 6 | FAIL centre,reversal |
| game fexp | 3 -> 3 | 1.2 -> 1.2 | 3.3 -> 2.7 | 1.3 -> 2.3 | 1.3 -> 1.3 | 1.7 / 4.7 -> 2.0 / 4.0 | 0 -> 0 | 26.7 -> 36.0 | 7.4 -> 5.7 | 4 -> 8 | FAIL centre,reversal |
| end fexp | 3 -> 3 | 1.2 -> 0.7 | 3.6 -> 2.7 | 1.3 -> 2.0 | 1.7 -> 1.0 | 1.3 / 2.7 -> 1.3 / 2.0 | 0 -> 0 | 28.0 -> 29.0 | 5.7 -> 3.5 | 6 -> 5 | FAIL reversal |
| first hcol | 3 -> 3 | 0.7 -> 0.6 | 1.9 -> 2.1 | - -> - | - -> - | 1.2 / - -> 5.2 / - | 0 -> 0 | 34.3 -> 30.7 | 2.3 -> 1.3 | 0 -> 0 | PASS |
| game hcol | 3 -> 3 | 1.0 -> 0.7 | 1.6 -> 1.3 | 1.7 -> 1.3 | 1.3 -> 0.7 | 2.0 / 3.0 -> 3.5 / 6.7 | 0 -> 0 | 28.3 -> 25.0 | 1.8 -> 2.0 | 0 -> 0 | PASS |
| end hcol | 3 -> 3 | 1.2 -> 0.7 | 2.7 -> 1.6 | 3.3 -> 2.3 | 1.0 -> 0.7 | 2.3 / 4.7 -> 3.5 / 7.4 | 0 -> 0 | 81.0 -> 30.0 | 2.8 -> 3.3 | 0 -> 0 | PASS |

### SE (UtttSE, light), after

| scenario | takes | board centre | corners | header | doors | centre / doors vs resting bottom | off | size step | size vs side | extra size reversals | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|
| first dexp | 3 | 1.1 | 3.2 | - | - | 4.0 / - | 0 | 7.0 | 6.0 | 0 | FAIL centre |
| game dexp | 3 | 1.0 | 3.5 | 1.0 | 1.0 | 2.0 / 4.5 | 0 | 5.5 | 6.5 | 0 | PASS |
| end dexp | 3 | 1.0 | 3.5 | 1.5 | 1.2 | 3.5 / 7.5 | 0 | 8.0 | 7.0 | 0 | PASS |
| first dcol | 3 | 0.8 | 1.0 | - | - | 4.0 / - | 0 | 8.0 | 1.4 | 0 | PASS |
| game dcol | 3 | 0.8 | 1.0 | 1.0 | 1.0 | 4.0 / 7.5 | 0 | 8.0 | 1.5 | 0 | PASS |
| end dcol | 3 | 0.8 | 1.1 | 1.0 | 1.0 | 4.0 / 7.5 | 0 | 8.0 | 1.4 | 0 | PASS |
| first fexp | 3 | 1.5 | 2.0 | - | - | 13.0 / - | 0 | 27.0 | 3.6 | 0 | FAIL centre |
| game fexp | 3 | 1.5 | 2.2 | 2.0 | 1.0 | 13.0 / 25.5 | 0 | 24.0 | 4.2 | 0 | FAIL centre,jump |
| end fexp | 3 | 1.0 | 2.5 | 1.5 | 1.0 | 13.0 / 25.5 | 0 | 26.5 | 4.0 | 0 | FAIL jump |
| first fcol | 3 | not reproduced: see below |||||||||||
| game fcol | 3 | not reproduced: see below |||||||||||
| end fcol | 3 | not reproduced: see below |||||||||||
| first hcol | 3 | not reproduced: see below |||||||||||
| game hcol | 3 | not reproduced: see below |||||||||||
| end hcol | 3 | not reproduced: see below |||||||||||
| first auto | 3 | 1.1 | 3.5 | - | - | 1.5 / - | 0 | 18.0 | 6.0 | 0 | FAIL centre |
| game auto | 3 | 1.0 | 3.5 | 2.0 | 1.0 | 1.2 / 2.0 | 0 | 26.5 | 6.0 | 0 | PASS |
| end auto | 3 | 1.0 | 3.5 | 1.5 | 1.0 | 1.5 / 2.5 | 0 | 23.5 | 5.5 | 0 | PASS |
| end dcol, dark | 1 | 0.8 | 1.0 | 1.0 | 1.0 | - | 0 | 8.0 | - | 0 | PASS |
| end auto, dark | 1 | 1.0 | 3.0 | 2.0 | 1.0 | - | 0 | 25.0 | - | 0 | PASS |

The SE's drags start inside the header (`GYOFF=12`): its expanded drawer reaches under the status bar, and a drag from the grab handle there pulled the system down instead.
No simulator flick lands on the SE's compact detent (a release mid-drawer springs back to expanded or goes on to minimised), and in the release takes the finder loses the board and doors for half a second while a partly covered red bar misreads, so the flick-collapse and hard-flick rows are not scored.
The flick expand's 13 / 25.5 pt against the resting bottom is the host's card: on the SE it lifts the whole card about 25 points while a finger drags up from compact and drops it at the release; the doors keep their 32.5 points above the card's bottom throughout.
Stage benchmark after (`film5/bench.sh`, 3 takes): 55.8 fps, largest gap 20 ms.
Release (device, not signed): no `dev.` strings, no ruler or host-move log strings, no em dashes in the app, the extension or UtttKit; foolish's Messages scheme builds.

### What is left

- Finger drags on the simulator: the corners step 4.2-4.7 pt and the centre 3-4 pt against the resting bottom in the frame of idb's first 40-54 pt step, the layout landing one frame after the host moved the drawer. The owner's device drag shows no such lag (the centre moves at most 0.5 pt a frame), so this is the simulator's touch injection; a device take will say.
- Flick expand: 4-8 extra size reversals of about 2 pt, while the finger holds and no new height is handed - the host shifting its card as the style changes. Not ours to lay out; worth a device take.
- `end dcol` centre 1.5 pt, first-open flick expand 1.2 pt: the same first-step lag.
- On the SE no flick lands on compact in the simulator (a release mid-drawer springs back to expanded or goes on to minimised), so its "flick collapse" row is the release that springs back.

### Filming the phone next time (devcap)

`shared/tools/devcap`: an iPhone's screen is offered to AVFoundation only in a process that set CoreMediaIO's `kCMIOHardwarePropertyAllowScreenCaptureDevices` itself.
The opt-in is per process, so ffmpeg's avfoundation input never sees the phone and QuickTime is a GUI; `devcap` (Objective-C only where AVFoundation needs it) sets it, lists devices and records.
Verified here: it builds (`make -C shared/tools/devcap`) and `devcap list` enumerates capture devices (the Mac's camera; no phone was connected).
A full record could not be proven from this session: macOS asks once for camera access for the terminal that runs it, and that prompt needs the owner.

When the phone is plugged in (unlocked, "Trust This Computer" answered):

```bash
cd /Users/alex/Dev/foolish-uttt
shared/tools/devcap/devcap.sh uttt/ios/Tools/ship.env devices            # the phone in devicectl and in AVFoundation ("ios-screen")
shared/tools/devcap/devcap.sh uttt/ios/Tools/ship.env install            # a DEBUG build (ruler compiled in) via devicectl, development signing
shared/tools/devcap/devcap.sh uttt/ios/Tools/ship.env ruler on           # touches dev.ruler in the App Group container on the phone
shared/tools/devcap/devcap.sh uttt/ios/Tools/ship.env film end_flick_1   # records until ^C; play the gesture, then ^C
```

`film` writes `~/devcap/<name>/take.mov`, tracks it (`motion_take.sh`), scores it (`motion score --bottom first`) and charts it (`motionplot.py`).
The first `film` may stop at the camera prompt: System Settings > Privacy & Security > Camera, allow the terminal app, run again.
A TestFlight or Release build has no ruler; `motion grid` still measures it (the drawer's top and the board's heavy grid lines):
`shared/tools/motion/motion_grid.sh take.mov out.grid [FROM TO]`.

## 18. The drawer-motion failures closed (2026-09-24, Pro Max and SE)

Takes, charts (x, y, against-anchor and width/height beside the drawer, one per take), scores and scripts are in the session scratchpad `film11/` (`takes/`, `charts/`, `scores/`, `table_pm.md`, `table_se.md`, `sweep.sh`, `table.sh`; `before/charts` links section 17's).
Every drag and flick row the owner's device can check was measured against the owner's 1.0(5) recording with `motion grid` (`film11/dev/all.grid`).

### The drag expand's first frame was ours, not the simulator's

The one-frame lag at the first touch step is in the owner's device video too: both flick expands from compact (18.503 s, 21.071 s) show a frame where the drawer's top moved 11.7 and 12.7 points, the board moved with it (the centre 11.2 and 12.2) and its side did not change (253.8, 254.1), then a 21-point size step the next frame.
The cause: the send hint's `onGrow` runs inside the overlay's `layoutSubviews`, in the same pass that lays the sheet out at the taller height, and `hideHintNow` ended with `CATransaction.flush()`.
The flush committed the half-done pass - the view already taller, the sheet still at compact - so the render server showed the stale layout for a frame (the Send hint gone, the board and the green bar riding the top).
`onGrow` now hides it without a flush; the pass's own commit takes it down in the same frame. After: the first frame's drawer step and the board's growth land together.

### What is left in a drag expand is the host's, and why

The remaining 4.0-4.3 point corner step is the RELEASE frame, every take.
While a finger holds the drawer, Messages shows the layer we last committed stretched to its card: up to 9 points taller than wide on a board we lay out square (the card narrowed about 2% while grabbed, and a frame ahead of the height it handed).
The log shows the board's bounds 370.909 square before and after the release; on screen it goes from 351 x 364 to 356 x 356 in that frame.
`motion` now counts board-size reversals over square frames alone as well (`sq_excess`, `MT_SQUARE_TOL`): every drag and flick expand reversal was in a stretched frame, 0 in square ones.
The corners are still scored on every frame, so the release frame stays in the table as a FAIL owned by the host.

### Flick expand: the clip reached the handed height, the drawer went past it

A flick's release is under-damped (damping 29.2 against a tap's 36.5, with the finger's velocity): the drawer overshoots the handed height by up to 6.4 points and comes back.
The sheet's clip ended at the laid-out bottom, so the riders on the drawer's bottom (doors, the ruler's green bar) were cut off through the overshoot; the partly cut bar read 2.3 points high and put the centre 1.2 points off.
`uttt_spring_past` (C, `uttt_anim_test`, mutation-checked) says how far the host's spring goes past its target, and the clip reaches that far through a followed growth.

### Measuring

- A bar cut by the frame's edge is not read (a drawer sliding off the bottom read its green bar 1.7 points high: the `end dcol` 1.5-point centre). A bar on the edge with all its rows in the frame is whole (an SE's expanded green bar sits on the last row). Both mutation-checked.
- `sq_excess` / `sq_skip` on the `size` line (above), mutation-checked. 81 motion checks.

### Pro Max (UtttRig, light), after

dexp 3 takes, fexp 3, the rest 2 (the regression pass). Before is section 17's table.

| scenario | takes | board centre | corners | header | doors | centre / doors vs resting bottom | off | size step | size vs side | extra size reversals (all / square frames) | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|
| first dexp | 3 | 0.7 | 4.3 | - | - | 3.0 / - | 0 | 9.3 | 8.7 | 0 / 0 | FAIL corners |
| game dexp | 3 | 0.7 | 4.0 | 1.7 | 1.3 | 3.0 / 6.7 | 0 | 14.7 | 8.0 | 1 / 0 | PASS |
| end dexp | 3 | 0.7 | 4.3 | 1.3 | 1.0 | 2.7 / 6.3 | 0 | 16.0 | 8.3 | 0 / 0 | FAIL corners |
| first fexp | 3 | 0.7 | 2.9 | - | - | 1.3 / - | 0 | 36.3 | 7.4 | 5 / 0 | PASS |
| game fexp | 3 | 0.7 | 3.0 | 1.7 | 1.0 | 1.3 / 2.7 | 0 | 34.6 | 6.4 | 6 / 0 | PASS |
| end fexp | 3 | 0.7 | 2.1 | 1.7 | 0.7 | 1.7 / 2.7 | 0 | 31.7 | 3.6 | 8 / 0 | PASS |
| first dcol | 2 | 0.7 | 0.8 | - | - | 3.5 / - | 0 | 14.3 | 1.0 | 0 / 0 | PASS |
| game dcol | 2 | 0.7 | 0.8 | 1.0 | 0.7 | 3.5 / 6.7 | 0 | 11.3 | 0.9 | 0 / 0 | PASS |
| end dcol | 2 | 0.7 | 0.7 | 0.7 | 0.7 | 1.2 / 2.7 | 0 | 11.3 | 0.8 | 0 / 0 | PASS |
| first fcol | 2 | 0.7 | 2.6 | - | - | 1.0 / - | 0 | 25.3 | 3.8 | 0 / 0 | PASS |
| game fcol | 2 | 0.7 | 2.9 | 1.4 | 0.7 | 1.3 / 2.3 | 0 | 23.3 | 4.7 | 0 / 0 | PASS |
| end fcol | 2 | 0.7 | 2.7 | 1.3 | 0.7 | 2.0 / 3.3 | 0 | 30.4 | 5.7 | 0 / 0 | PASS |
| first hcol | 2 | 0.7 | 1.6 | - | - | 1.8 / - | 0 | 39.3 | 3.3 | 0 / 0 | PASS |
| game hcol | 2 | 0.7 | 1.8 | 3.0 | 0.7 | 3.9 / 7.4 | 0 | 35.6 | 1.4 | 0 / 0 | PASS |
| end hcol | 2 | 0.7 | 2.1 | 1.3 | 0.7 | 2.0 / 2.7 | 0 | 30.7 | 1.3 | 0 / 0 | PASS |
| first auto | 2 | 0.7 | 3.0 | - | - | 2.0 / - | 0 | 26.7 | 5.3 | 0 / 0 | PASS |
| game auto | 2 | 0.7 | 2.7 | 1.3 | 0.3 | 1.8 / 4.0 | 0 | 31.4 | 5.3 | 0 / 0 | PASS |
| end auto | 2 | 0.5 | 2.7 | 1.7 | 0.7 | 1.7 / 2.7 | 0 | 30.0 | 5.7 | 0 / 0 | PASS |

### SE (UtttSE, light), after

| scenario | takes | board centre | corners | header | doors | centre / doors vs resting bottom | off | size step | size vs side | extra size reversals (all / square frames) | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|
| first dexp | 2 | 1.1 | 3.5 | - | - | 1.1 / - | 0 | 8.5 | 6.5 | 0 / 0 | FAIL centre |
| game dexp | 2 | 1.0 | 3.5 | 1.5 | 1.1 | 1.0 / 2.1 | 0 | 8.0 | 6.9 | 0 / 0 | PASS |
| end dexp | 2 | 1.0 | 3.5 | 1.5 | 1.1 | 1.0 / 2.0 | 0 | 9.0 | 6.9 | 0 / 0 | PASS |
| first dcol | 2 | 0.6 | 1.0 | - | - | 4.0 / - | 0 | 8.0 | 1.6 | 0 / 0 | PASS |
| game dcol | 2 | 0.8 | 1.0 | 1.0 | 1.0 | 4.0 / 7.5 | 0 | 8.0 | 1.4 | 0 / 0 | PASS |
| end dcol | 2 | 0.5 | 1.1 | 1.0 | 1.0 | 2.1 / 4.0 | 0 | 8.0 | 1.6 | 0 / 0 | PASS |
| first fexp | 2 | 1.0 | 2.8 | - | - | 1.5 / - | 0 | 24.0 | 7.0 | 0 / 0 | PASS |
| game fexp | 2 | 1.0 | 2.0 | 1.5 | 1.0 | 1.5 / 3.0 | 0 | 25.0 | 7.0 | 0 / 0 | PASS |
| end fexp | 2 | 1.0 | 2.2 | 1.5 | 1.5 | 1.5 / 2.0 | 0 | 26.0 | 7.0 | 0 / 0 | PASS |
| first fcol | 2 | 1.0 | 1.2 | - | - | 1.0 / - | 0 | 6.0 | 1.6 | 0 / 0 | PASS |
| game fcol | 2 | 0.5 | 1.2 | 1.0 | 1.0 | 0.5 / 1.0 | 0 | 6.0 | 1.6 | 0 / 0 | PASS |
| end fcol | 2 | 1.0 | 1.1 | 1.0 | 1.0 | 1.0 / 1.0 | 0 | 6.0 | 2.1 | 0 / 0 | PASS |
| first hcol | 2 | - | - | - | - | - / - | 0 | - | - | - / - | PASS |
| game hcol | 2 | - | - | 2.0 | - | - / - | 0 | - | - | - / - | PASS |
| end hcol | 2 | - | - | 1.0 | - | - / - | 0 | - | - | - / - | PASS |
| first auto | 2 | 1.0 | 3.0 | - | - | 1.0 / - | 0 | 24.5 | 5.5 | 0 / 0 | PASS |
| game auto | 2 | 1.0 | 3.5 | 2.0 | 1.0 | 2.0 / 2.0 | 0 | 25.5 | 5.5 | 0 / 0 | PASS |
| end auto | 2 | 1.0 | 3.5 | 1.5 | 1.0 | 1.5 / 2.0 | 0 | 22.5 | 5.5 | 0 / 0 | PASS |

- SE flick collapse: no simulator flick lands on the SE's compact detent (tried 0.1-0.3 s over 60-280 points: all spring back to expanded or go on to minimised). A slow drag released mid-drawer (0.5 s, 250 points) does land on compact through the host's release animation, which is the owner's device case; that is the `fcol` row.
- SE hard flick: the board and bars leave the frame in the take's first frames (the swipe starts where iOS's own top-edge gesture competes), so only the header is scored; no board data, not a pass on the board.
- SE `first dexp` centre 1.1: the SE is 2x and the finder reads at half resolution, a 1-point grid; the steps are 1.0-1.12 alternating sign, the finder's floor, not a move.

Stage benchmark (`film5/bench.sh`, SE, 3 takes): 56.5-57.1 fps, largest gap 20 ms.
`make -C uttt/c run asan ios-smoke` and `make -C shared/tools/motion test` green.
Release (simulator, not signed): no `dev.ruler`, `host-move` or em dash in the app, the extension or UtttKit.
`shared/` changed only in `shared/tools/motion` (no app code), so foolish's app build is untouched.

### What is left

- Drag expand corners 4.0-4.3 at the release frame: the host's grab stretch letting go. A device take (devcap, section 17) with the ruler would say whether the phone stretches the same way.
- SE hard flick: needs a start point that iOS's top-edge gesture does not claim before the board can be scored.

## 19. TestFlight 1.0(6) feedback: doors, the you-are O, the win line, the beads (2026-09-24)

Images are in the session scratchpad `b6/`; `make -C uttt/c look` builds `tools/uttt_look`, which draws the pieces at a phone's scale the way Core Graphics fills them and prints numbers.

### Again and Copy code are one width

`uttt_sheet` gave Copy code 45% of the door row and Again the rest.
The row less one gap is now halved and both doors take that one number.
`uttt_anim_test` walks every drawer height 240-900 on every width 320-440 and asserts equal widths (mutation-checked).

### The door's horizontal borders were thinner: one constant, two coordinate systems again

The door comes back in 0..1 of its own width AND height and the host stretches it to w x h, but `emit_flip` built each ribbon in those unit coordinates with its width as a fraction of w.
So every stroke's thickness along y came back multiplied by h/w: measured on a 141.5 x 46 door (an SE with Copy code), top and bottom 3.6 px at 3x, left and right 9.8 px.
The earlier mirroring made top match bottom and left match right, and could not make the pairs match each other.
Ribbons are now built in points and only their points divided by the size.
What was left (top 8.97 / left 9.83 px at 3x) is rough.js's two passes parting further on the short sides; `even_edges` gives each side pair the width that lays one shared amount of ink across it.
After: 141.5 x 46 at 3x top 9.46 / left 9.35 px, at 2x 6.38 / 6.27 px; on the simulator's own Core Graphics render (SE, 2x) Again and Copy code both measure top 5.89 / bottom 5.89 / left 5.88 / right 5.88 px.
`ios-smoke` cuts each edge across in points at nine places on doors 100-430 points wide and holds all four within 0.5 px at 3x (worst 0.30); both halves of the fix mutation-checked red.
Side effect: the hachure lines were squashed the same way and are now their true 1.4 points in every direction, so the fill reads a little denser, matching the rulebook square.

### The you-are O no longer cuts across itself

At its 88-unit size the O's roughness is 2.66, and rough.js's closing overlap then sweeps up to two thirds of a turn on a few points: for about one seed in five the curve cut a chord through the circle.
`uttt_o_in_ring` asks whether every sample lies within 15% of the O's own median radius; `uttt_mark_seed` walks a fixed sequence from the game seed to the first O that does (31% pass, at most 26 tries in 200,000 seeds), a pure function of the seed so both phones agree.
`ios-smoke` checks 20,000 game seeds off the display list (worst stray 0.161 of the radius; 0.779 without the walk).

### The win line was ruled

rough.js bows a line by bowing x maxRandomnessOffset x length / 200, and the win line converted both the offset and the length to the unit board, so the bow came out squared-small.
Over 300 won games its centreline wandered 0.12% of the board in the straightest game and 0.63% on average.
It is now drawn in a hundred-unit board (roughness 1.5, bowing 2, offset 1.5) and scaled back once: least 1.6%, mean 3.7%, most 6.9%.
`uttt_anim_test` requires 1-8% in every game; the old line fails it.
The grid lines use the same `mro_for` x length arithmetic and so have no bow either; not changed here (their wander is endpoint jitter).

### The beads: options, nothing shipped

`uttt_ink` lays a quad per segment and a disc per sample, all at the stroke's alpha, so at every sample three translucent shapes overlap: 1-(1-a)^3 instead of a (.8 becomes .99; a minor line's .5 becomes .875).
rough.js's second pass adds a broad overlap on top.
Rendered from the owner's game (`NK2JIG6A6YIFDLRPPZ6Z5QGXZASJCBSINMIQ`, 320 points at 3x) with `uttt_look board CODE 320 3 MODE`, strokes recovered from the quad chain (256 strokes, 25,360 polygons): `b6/item5_options_sheet.png` and `b6/opts/crop_*_4x.png`.

- a) opacity 1: no beads, but every mark goes solid and the won-block fade, the grid's .5 and the hierarchy of weights go with it. Free.
- b) each stroke opaque into a transparency layer, the layer at the stroke's alpha: no beads; pass-over-pass and stroke crossings still darken like ink over ink. Loses the per-segment alpha grain (the stroke takes its mean). About 256 `CGContextBeginTransparencyLayer`s per board, each a bbox-sized offscreen: several times the fill cost of today, and every per-stroke Core Animation layer would need group opacity (`allowsGroupOpacity`, offscreen pass per layer).
- c) darken blend, colour pre-mixed over the paper: no beads, but a pre-mix against a flat paper loses the paper grain through the ink, and every mark comes out paler. Cheap in Core Graphics (`.darken`), awkward for the bubble bake and wrong on a textured sheet.
- d) per-stroke coverage by MAX: like b and keeps the alpha grain, but Core Graphics has no MAX-coverage mode, so it means the kernel rasterising itself.
- e) one outline polygon per stroke (the centreline and per-sample half-widths, bisector normals, round caps), filled once at the stroke's mean alpha: no beads, crossings still darken, about 256 polygons instead of 25,360, identical on both phones because it is geometry from C, no change to Core Animation or the bake. Loses the per-segment alpha grain; keeps the width modulation.
- f) one layer for the whole sheet with MAX alpha: nothing darkens, not even two strokes crossing, which reads as printed rather than drawn.

Recommendation: e).
It fixes the cause (the pen overlapping itself) in the one place geometry lives, makes the board a hundredth of the polygons (cheaper to fill, to cache and to animate), and keeps the ink-over-ink crossings that make it read as a pen.
If the alpha grain is missed, it can come back as a width-only grain or as a paper-side texture; b) is the fallback if the grain matters more than the cost.

## 9. Device findings, 2026-09-25 (for the next session)

### Again: no live arrival after the invitation

Seen: at a game's end, tap the end bubble, tap Again, send the invitation from the compact drawer and leave it up. The opponent's reply shows in the thread, and the open drawer never hears of it.

Why: an open drawer is bound to the datasource of the bubble that opened it, and the host delivers `didReceive` only for a message in that bubble's MSSession (foolish's host-binary notes, `ios/FoolishMessages/MessagesViewController.swift`, in `didStartSending`). Again sets `freshSession = true`, so the invitation goes out in a new `MSSession()` (`sessionFor`), and the drawer stays bound to the finished game's session. The reply lands in the new session and there is nobody to tell. The `wasUnbound` dismiss does not fire, because the drawer is bound, only to the wrong game.

Decision (owner): **do not set `freshSession = true` in `again()`.** The invitation stays in the finished game's session, so the reply reaches the open drawer. The cost is that Messages collapses the finished game's last bubble to its caption; the replay link still holds the game. Check what else reads `freshSession` and `draftIsNewGame` (the `newest`/`current` routing that ranks a new game over the tapped one) before removing it.

### A sent move comes back as a staged one

Seen: send a move, and a move is still staged in the field; sending that one "overwrote" the first.

Why (from the code, not yet from a log): the insert watchdog outlives the send. Every insert arms `watchSilence`; if the host has not answered within 500 ms (`MS_INSERT_SILENCE_MS`) in compact, it inserts the same `MSMessage` again, up to 10 times. The only guards are `stageGeneration == generation` and `landedGeneration != generation`. `didCancelSending` bumps `stageGeneration` (line ~480); **`didStartSending` does not**. So when ChatKit puts the bubble in the field but answers late or never (the case the `late answer` log line exists for), and the human presses Send inside that window, the next watchdog tick finds its stage still current and not landed, and re-inserts the bubble that was just sent. It is the same message in the same session, so sending it again makes Messages collapse the first copy to its caption: the "overwrite". The 0.35 s retry after an insert error, and the send door, hang off the same generation and fail the same way.

Fix: in `didStartSending`, void every in-flight stage, as the cancel does: `stageGeneration += 1` (and `doorInsert = nil`, which it already does). Every watchdog, retry and door checks the generation, so all of them stand down.

To confirm from a device: the Diagnostics log should show `send` followed by `insert attempt N got no answer; retrying` and a further `insert attempt`.
