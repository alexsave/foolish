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

**Still wrong: the board.** It sits up to ~77pt below the drawer's centre early in the slide and steps back to it (77 -> 29 -> 0) as SwiftUI re-lays the nested host out; at rest it is centred (+0.2pt).
The riders are heard before the nested hosts are laid out at the compact size (logged: every rider 440x840 at the flip), and the rebuild on relayout (`laidOut`) did not remove the steps.
Next: log the board rider's bounds and the keyframes it was built with on each rebuild, and check whether the ride closure in use at the flip is the expanded render's.
Not yet measured after: dark, manual drag collapse/expand (the board is now always in a nested host), the other scenarios the owner asked for (join, first-open, end, tap-to-open, arrival, final move, Again).

### The defects

1. The send hint hides in the frame it is sent or the drawer grows: the overlay's layer is hidden and committed at once (`hideHintNow`), and a drawer laid out taller than it rested counts as growing (a drag hands a height every frame; willTransition only comes at the release). SendHint takes `hidesAtOnce` (foolish keeps its fade).
2. `SendHintInk.white`: white arrow and caption ringed in the send blue; foolish keeps `.blue(outline:)`.
3. The first board of a process is painted synchronously at 1x (a ninth of the pixels) so the first frame has the lines, and sharpened off the main thread.
4. The words are set twice, in the column beside the ink and in the band, and the kernel shows each only where it fits (`SHEET_COLUMN_NEED`, `SHEET_BAND_NEED`), crossfading - no height squeezes them.
5. The headline and subline speak of the position one ply back until the kernel's frame says the ink has landed (`uti_say_before`).
6. The settlement half: a won block's big mark falls (780 ms) and then the win line draws (500 ms), at Send (channel B, `UTTT_CH_SETTLE`) and after the ink on an opened or arrived bubble; never at stage. `uttt_draw_settle` + `uti_draw_under` without them.
7. With a bubble staged the strip's right column starts under the hint (`SHEET_HINT_ROOM`).

Defects 1-7 are built and unit-tested (C tests mutation-checked: the push curve, the rest, the words crossfade, the hint room, the settlement order and composition), but not yet filmed.
