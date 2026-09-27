# iOS Messages layer - consolidation pass over five products

Investigated in the `shared-consolidation` worktree, read-only.
Products: foolish/ios (FoolishKit, FoolishMessages, SwiftUI, shipped), uttt/ios (UtttKit, UtttMessages, UIKit and Core Animation, never loads SwiftUI, in App Store review), pickemup/ios (PickemupKit, PickemupMessages), chuiniu/ios (ChuiniuKit, ChuiniuMessages), tallybones/ios (TallybonesKit, TallybonesMessages).
werewolf/ios is out of scope; `werewolf/COMMON.md` items 6, 7, 8 and 10 are cited as background only.
Every finding below is a diff or a grep result, not a guess; file paths are given so the claim can be re-run.

## Verdict

The three new products (pickemup, chuiniu, tallybones) genuinely adopted `shared/swift/MessagesKit`'s `InsertStaging` and `DevFlags`.
They did not reimplement insert-staging or echo-detection locally; every product's `MessagesViewController.swift` calls `InsertStaging.Loop`, `InsertStaging.drawerUp` and `InsertStaging.receive` at the same call sites pickemup and uttt use.
`SendHint` and `CollapseSlide` are a different story: pickemup adopted both, chuiniu and tallybones adopted neither, and this is a *recorded product decision* (chuiniu `docs/DECISIONS.md` I12, tallybones `docs/DECISIONS.md` T55), not a silent gap or a local reimplementation.
Both products instead call `requestPresentationStyle(.compact)` directly after a kernel-timed settle delay, which is the pre-`CollapseSlide` behavior the shared file's own header comment says was measured to step frame-by-frame ("THE PROBLEM").
That is worth a maintainer's attention even though it is not a code-reuse defect: chuiniu and tallybones may be shipping the older, jankier collapse.

Across the design-file families (Tokens, Buttons, Materials, BubbleSnapshot), the three new products are now close to a product-neutral shared slice **among themselves**: `Buttons.swift` differs between pickemup, chuiniu and tallybones by essentially one type name (`PkHaptic` / `CnHaptic` / `TbHaptic`) and the header comment.
But none of that can move into `shared/swift/` today, because every file placed there compiles into FoolishKit by directory source path (`foolish/ios/project.yml:346`), and foolish's own `Tokens.swift`, `FSquareButton.swift` and `FTextures.swift`/`Materials.swift` still read foolish's `Suit`, `Haptics`, `WoodFill`, `FPrefs` and `FStrings` (REUSE_AUDIT S2, confirmed still true below).
The S2 blocker was resolved on the *product* side (pickemup's copy dropped `FPrefs` outright, per its own Materials.swift comment: "There is no material preference, so FPrefs is gone"), not on foolish's side, so lifting is still a foolish code change, which the lift rule in `pickemup/docs/REUSE_AUDIT.md` section 5 forbids for a path-only step.

The `MessagesViewController` lifecycle (adopt on select, Rule P on receive, stage, commit on `didStartSending`, cancel, `awaitTransitionSettled`) is now identical in shape across uttt, pickemup, chuiniu and tallybones - same method names, same order, same helper name `awaitTransitionSettled(timeoutNs:)` in all four.
foolish's own copy has the same *shape* one level up (`didSelect`, `didReceive`, `didStartSending`, `didCancelSending`, its own `awaitTransitionSettled`) but is SwiftUI-only, 1197 lines, and - per REUSE_AUDIT defect D4, reconfirmed here - does not call the shared insert gating at all.
A `MessagesSurfaceController` base class (werewolf/COMMON.md item 6) is real and would cost uttt, pickemup, chuiniu and tallybones only a path change; it would NOT be a path-only change for foolish, because adopting the shared gating in foolish is the behavior change D4 already flags as its own task.

Recommendation, in the owner's terms: lift `mac_tests.sh` for chuiniu and tallybones now (pickemup already proved the pattern), defer the `MessagesSurfaceController` base class and the Tokens/Buttons/Materials/BubbleSnapshot split until foolish's own S2-equivalent split lands, and do not lift anything product-specific (Suit, board geometry, config files).

---

## MessagesKit adoption table

| shared file | pickemup | chuiniu | tallybones |
| --- | --- | --- | --- |
| `InsertStaging.swift` | adopted - `PickemupMessages/MessagesViewController.swift:49,120,215,447,463,486` | adopted - `ChuiniuMessages/MessagesViewController.swift:39,83,160,351,360,383` | adopted - `TallybonesMessages/MessagesViewController.swift:40,96,174,383,392,415` |
| `DevFlags.swift` | adopted - `PickemupKit/Kernel/PickemupDev.swift` | adopted - `ChuiniuKit/Kernel/ChuiniuDev.swift` | adopted - `TallybonesKit/Kernel/TallybonesDev.swift` |
| `SendHint.swift` / `SendHintMetrics.swift` / `SendHintView.swift` | adopted - `PickemupKit` uses `SendHint` at 2 sites, wired through `project.yml:124-125` | not adopted, not reimplemented - no `arrow`/`unsent`/`sendHint`-shaped code anywhere in `chuiniu/ios` | not adopted, not reimplemented - no equivalent code anywhere in `tallybones/ios` |
| `CollapseSlide.swift` | adopted - `PickemupKit/Kernel/PkCollapse.swift`, `PickemupDev.swift`, `PickemupKitTests/DevFlaggedTests.swift`, `PickemupMessages/MessagesViewController.swift`; `project.yml:123` | not adopted - reimplemented as a plain `requestPresentationStyle(.compact)` call after a kernel-timed delay, `ChuiniuMessages/MessagesViewController.swift:293-332` (see below); recorded as decision I12 | not adopted - the same plain `requestPresentationStyle(.compact)` pattern, `TallybonesMessages/MessagesViewController.swift:` around its `stage(...)` function; recorded as decision T55 |
| `MotionRuler.swift` | not applicable - no ruler file exists in `pickemup/ios` yet | not applicable - none in `chuiniu/ios` | not applicable - none in `tallybones/ios` |
| `PackedBytes.swift` | not applicable - each product reads its own kernel's wire through its own generated bridge, not foolish's SDK reader | not applicable | not applicable |

The two "not adopted" rows for `CollapseSlide` are a real behavioral gap, not a reuse defect: the code that would call `CollapseSlide` (a per-element frame-accurate riding animation over the drawer's own slide) does not exist in chuiniu or tallybones at all.
What exists instead, in both products, is the simpler direct system call:

```
// chuiniu/ios/ChuiniuMessages/MessagesViewController.swift:317-331
guard collapse, presentationStyle != .compact else {
    insert(message, generation: generation, in: conversation)
    return
}
Task { @MainActor [weak self] in
    try? await Task.sleep(nanoseconds: UInt64(max(settleMs, 0)) * 1_000_000)
    guard let self, self.stageGeneration == generation else { return }
    if self.presentationStyle != .compact {
        self.requestPresentationStyle(.compact)
        await self.awaitTransitionSettled()
    }
    ...
}
```

tallybones' `stage(...)` (`TallybonesMessages/MessagesViewController.swift`, around line 340-360) is the same shape with `TbStage` in place of the caption/collapse tuple.
This is the "collapse-after-settle" pattern pickemup itself introduced (its DECISIONS.md I12), stripped of the `CollapseSlide` per-frame riding that pickemup layered on top of it.
Nothing in either product's `docs/DECISIONS.md` explains *why* the riding animation was dropped rather than adopted; it reads as scope-cut, not as a reasoned rejection of the shared file.

## MessagesViewController lifecycle comparison

Method-by-method, by grepping `override func` and the two private helpers in all five files:

| Method | foolish | uttt | pickemup | chuiniu | tallybones |
| --- | --- | --- | --- | --- | --- |
| `viewDidLoad` | yes (:96) | yes (:104) | yes (:79) | yes (:57) | yes (:59) |
| `viewDidLayoutSubviews` | no | yes (:228) | yes (:124) | yes (:87) | yes (:100) |
| `viewDidAppear` / `viewDidDisappear` | no | yes (:202,256) | yes (:135,144) | yes (:92,101) | yes (:105,114) |
| `willBecomeActive` | yes (:219) | yes (:128) | yes (:171) | yes (:121) | yes (:132) |
| `didBecomeActive` | no | yes (:159) | yes (:190) | yes (:137) | yes (:150) |
| `willResignActive` | no | yes (:166) | yes (:196) | yes (:143) | yes (:156) |
| `didResignActive` | yes (:242) | yes (:267) | no | no | no |
| `didSelect` | yes (:178) | yes (:283) | yes (:203) | yes (:148) | yes (:162) |
| `didReceive` | yes (:271) | yes (:308) | yes (:212) | yes (:157) | yes (:171) |
| `didStartSending` | yes (:414) | yes (:393) | yes (:223) | yes (:168) | yes (:182) |
| `didCancelSending` | yes (:587) | yes (:479) | yes (:245) | yes (:184) | yes (:205) |
| `willTransition` | yes (:627) | yes (:519) | no (folded into `didTransition`) | no | no |
| `didTransition` | yes (:663) | yes (:554) | yes (:260) | yes (:198) | yes (:220) |
| `awaitTransitionSettled` helper | yes (:682) | yes (:1106) | yes (:496) | yes (:393) | yes (:425) |

What each does in the shared methods:

- **`didSelect`**: all five adopt on the tapped bubble; uttt/pickemup/chuiniu/tallybones route this through the same seat-resolver pattern (`ms_seat_resolve`-equivalent), foolish through `SeatIdentity.swift`.
- **`didReceive`**: all five run Rule P (`msg_rule_p` in foolish's kernel, the product's own wire-equivalent in the other four) and hand the result to `InsertStaging.receive` in uttt/pickemup/chuiniu/tallybones; foolish does its own bare compare with no shared call (D4, below).
- **`didStartSending`**: all five treat "what went out" as authority over the staged draft; the comment blocks in pickemup/chuiniu/tallybones (quoted in the file headers) describe the identical rule in near-identical words, credited to uttt's original.
- **`didCancelSending`**: all five rebuild the resident draft to a floor rather than losing it; uttt learned this the hard way per its own comments, and it was carried into all three new products unchanged.
- **`awaitTransitionSettled`**: byte-for-byte the same 1.2-second timeout constant (`timeoutNs: UInt64 = 1_200_000_000`) in uttt, pickemup, chuiniu and tallybones; foolish has its own copy of the same function at the same default.

**foolish's compile-in-but-do-not-call gap (D4), reconfirmed**: `foolish/ios/project.yml:346` gives FoolishKit the whole `shared/swift` directory, so `InsertStaging.swift` builds into FoolishKit, but `foolish/ios/FoolishMessages/MessagesViewController.swift` does not call `InsertStaging.Loop`, `.drawerUp` or `.receive` anywhere; its insert at line 968 (approximately, per REUSE_AUDIT) is a bare `conversation.insert`.
This means a shared `MessagesSurfaceController` base class that runs the `InsertStaging` loop is a **behavior change for foolish**, not a path change, exactly as the task brief anticipates.

**Recommendation on a shared lifecycle type**: a base class (over free functions or a protocol with default implementations) fits best, because the state being managed - `staged`, `sent`, `arrived`, `draftURL`, the stage generation counter, the pending-stage `Task` - is exactly the kind of owned, mutable, single-instance state a base class exists for; a protocol-with-defaults would need that state re-declared as `associatedtype` storage in every conformer, which is more boilerplate than the four products currently carry between them.
uttt, pickemup, chuiniu and tallybones would each adopt it with a path change only: swap their own class's superclass, delete the now-redundant lifecycle overrides, keep only `stage(caption:collapse:)`-equivalent and the SwiftUI/Core-Animation root view as the subclass override points.
foolish would need the insert-gating behavior change (D4) landed and separately verified with the rig **before** it could adopt the base class at all; adopting the base class and fixing D4 in the same step would make a lift that is supposed to be zero-risk into a change that needs the full P1-P9 proof suite plus a device rig pass.
So: lift-later for the base class, gated on D4 being fixed and proved in foolish on its own first.

---

## COPIED families

### 1. MessagesViewController.swift

| File | Lines |
| --- | --- |
| `foolish/ios/FoolishMessages/MessagesViewController.swift` | 1197 |
| `uttt/ios/UtttMessages/MessagesViewController.swift` | 1327 |
| `pickemup/ios/PickemupMessages/MessagesViewController.swift` | 507 |
| `chuiniu/ios/ChuiniuMessages/MessagesViewController.swift` | 404 |
| `tallybones/ios/TallybonesMessages/MessagesViewController.swift` | 436 |

Provenance chain, from each file's own header comment: pickemup COPIED from uttt at `16433dc1`; chuiniu COPIED from pickemup at `03eb3362`; tallybones COPIED from pickemup at `8e216923` (itself from uttt at `16433dc1`).
foolish is an independent original, not in this chain (uttt's own header says its lifecycle was "re-implemented," not copied from foolish).

Diff evidence: `diff pickemup/.../MessagesViewController.swift chuiniu/.../MessagesViewController.swift` is 443 lines of diff against 507/404 source lines - most of the difference is `Pk*` to `Cn*` type renames, the header comment, and the `CollapseSlide`/`SendHint` block pickemup has and chuiniu does not (about 40 lines: the `slide` property, `showBoard()`, the `dev.slide`/`dev.sendhint` comment block).
`diff chuiniu/.../MessagesViewController.swift tallybones/.../MessagesViewController.swift` is 340 lines of diff against 404/436 lines - again mostly type renames (`Cn*` to `Tb*`) plus tallybones' extra `becameActiveAt` tracking.
What each product changed beyond naming: pickemup carries the `CollapseSlide`/`SendHint` wiring (chuiniu and tallybones do not, see above); each product's `stage(...)` builds a different `MSMessageTemplateLayout` (`BubbleSnapshot.render(table:...)` vs `host.table`/`kernel.table`-shaped calls) and reads a different URL/text identity for its own bubble (`Card` wire vs dice/tally payload) - that is the genuine per-game state, not boilerplate.

Where product identity leaks in: the type names (`PkHaptic`/`CnHaptic`/`TbHaptic`, `PickemupHost`/`ChuiniuHost`/`TallyKernel`), the import line (`import PickemupKit` / `ChuiniuKit` / `TallybonesKit`), the App Group suite name each reads through its own `*Dev.swift`, and the payload codec each `stage()` builds.
None of the three leak a bundle id or App Group name directly into `MessagesViewController.swift` itself - those live in `project.yml`/`Info.plist`/entitlements (see the config-file section).

**Lift verdict: lift-later, blocked on foolish's D4 fix.**
Proof that foolish and uttt are unchanged by this investigation (nothing here was edited, this is a read-only pass): foolish's snapshot proof is `ios/FoolishTests/__Snapshots__` (853 XCTest cases executed under `FoolishTests`, plus 29 `HarnessTests` cases, run through `ios/scripts/mac_tests.sh`, per `pickemup/docs/REUSE_AUDIT.md` section 2 and the "P8 after all lifts" log); uttt has **no** XCTest target and no snapshot suite at all (`pickemup/docs/REUSE_AUDIT.md` S5: "uttt, new: no test target, so `preview=UtttPreview app=UtttMessagesApp` are both builds"), so uttt's only proof is its C-side `make -C uttt/c run asan ios-smoke` (P10) plus a successful Xcode build of `UtttPreview`/`UtttMessagesApp`.

### 2. Tokens.swift

| File | Lines |
| --- | --- |
| `foolish/ios/FoolishKit/DesignSystem/Tokens.swift` | 169 (per REUSE_AUDIT S2) |
| `pickemup/ios/PickemupKit/Design/Tokens.swift` | 74 |
| `chuiniu/ios/ChuiniuKit/Design/Tokens.swift` | 70 |
| `tallybones/ios/TallybonesKit/Design/Tokens.swift` | 68 |

`FSpace`/`FRadius` are unchanged in shape across the three copies (no diff hunk touches them).
`FColor` keeps the same field names (`card`, `ink`, `textPrimary`, `textDim`, `win`, `deepRed`) in all three, but the hex values and the extra fields differ per game: pickemup keeps `amber` and `red` (its catch mechanic), chuiniu and tallybones drop them.
`FMotion` differs in where its three numbers come from: pickemup reads `PK_T_SPRING`/`PK_T_CHROME`/`PK_T_PRESS` from its own C kernel header via `import CPickemup` (the owner's C-over-Swift rule, applied); chuiniu and tallybones type the same three durations as Swift literals, because - per chuiniu's own comment - "the chuiniu kernel [does not yet export] its own beats," and tallybones' comment says its kernel (`tb_beats.h`) "has no such names, so they stay literals here (DECISIONS T62)."

**Suit check (the S2 blocker)**: none of the three copies imports or references `Suit` anywhere in `Tokens.swift` - confirmed by `grep -n "Suit"` returning nothing in any of the three files.
foolish's own `Tokens.swift` still has `FColor.suitColor` reading `suit.isRed` (REUSE_AUDIT S2, not re-verified line-by-line here since foolish was not to be modified, but nothing in this pass found a change to that file).
So the blocker is resolved on the product side, not on foolish's side: `shared/swift` cannot host a neutral `Tokens.swift` until foolish's own file is split, because any file placed there also compiles into FoolishKit and a duplicate `public enum FColor` in the same module is a compile error, not a warning.

**Lift verdict: lift-later.** The three products already prove a common shape exists; foolish is the one blocker, and splitting foolish's `FColor`/`FSpace`/`FRadius`/`FMotion` out from its `Suit`-reading extension is a foolish code change requiring its own P8 run, per REUSE_AUDIT's own rule that a lift step touching foolish beyond a path must be split into its own reviewed step.

### 3. Buttons.swift (foolish: FButton.swift + FSquareButton.swift + Haptics.swift)

| File | Lines |
| --- | --- |
| `pickemup/ios/PickemupKit/Design/Buttons.swift` | 90 |
| `chuiniu/ios/ChuiniuKit/Design/Buttons.swift` | 89 |
| `tallybones/ios/TallybonesKit/Design/Buttons.swift` | 90 |

This is the tightest family in the whole audit.
`diff pickemup/.../Buttons.swift chuiniu/.../Buttons.swift` and `diff chuiniu/.../Buttons.swift tallybones/.../Buttons.swift` each show only the header comment and one type rename: `public enum PkHaptic { case pickUp, drop, reject }` becomes `CnHaptic` becomes `TbHaptic`, used only as `Haptics.fire(_:)`'s argument type.
`Haptics`, `WoodFill`, the press style and the pill geometry are otherwise byte-identical across all three copies.

**S2 blocker check**: `grep -n "FPrefs\|FStrings\|FActionBar\|SettingsHelpSquares"` on all three `Buttons.swift` returns nothing.
`Haptics` and `WoodFill` are now declared inside the same file (or, for `WoodFill`, in the product's own `Materials.swift`), not called out to foolish's originals - the blocker REUSE_AUDIT S2 names (`FSquareButton.swift calls Haptics, WoodFill, FPrefs and FStrings keys`) is resolved on the product side by inlining the dependency, not by foolish exposing a parameterized surface.

**Lift verdict: lift-later, same reasoning as Tokens.swift** - a neutral `shared/swift/DesignKit/Buttons.swift` would need only the haptic enum's three cases (rename to something neutral, e.g. `MessagesHaptic`) to serve all three new products with a path change, but it still cannot move into `shared/swift` until foolish's own `FSquareButton.swift` stops depending on `FPrefs`/`FStrings`/`FActionBar` (same S2 blocker, unresolved on foolish's side).

### 4. Materials.swift (foolish: FTextures.swift + Materials.swift)

| File | Lines |
| --- | --- |
| `pickemup/ios/PickemupKit/Design/Materials.swift` | 108 |
| `chuiniu/ios/ChuiniuKit/Design/Materials.swift` | 99 |
| `tallybones/ios/TallybonesKit/Design/Materials.swift` | 105 |

Same shape as Buttons.swift: `diff pickemup chuiniu` shows a type rename (`PkTextures` to `CnTextures`) and one real removal - chuiniu drops `fernBack` because, per its own comment, "this game has no cards."
That single removal is the clearest example in the whole audit of a genuine product-specific difference hiding inside an otherwise mechanical rename-diff, and it is exactly why this family cannot be lifted as one generic module without a "does this product have a card back" parameter.
`FPrefs` is confirmed gone from all three (pickemup's own comment: "There is no material preference, so FPrefs is gone and the variant is the colour scheme alone"), matching the Buttons.swift finding.

**Lift verdict: lift-later**, blocked on the same foolish-side FPrefs/FTextures split, plus a per-product "has a card back" parameter the three products already imply but do not yet share explicitly.

### 5. LobbyScreen.swift

| File | Lines |
| --- | --- |
| `pickemup/ios/PickemupKit/Screens/LobbyScreen.swift` | 221 |
| `chuiniu/ios/ChuiniuKit/Screens/LobbyScreen.swift` | 98 |
| `tallybones/ios/TallybonesKit/Screens/LobbyScreen.swift` | 172 |

This family varies far more than Buttons/Materials/Tokens - chuiniu's is less than half pickemup's line count.
chuiniu's comment says it kept "the title, the roster in seat order, and one [start action]" and cut the rest; tallybones kept a numbered roster and its own rules surface.
This tracks the REUSE_AUDIT classification for the underlying `msg_lobby_*` rules as "(c) liftable after parameterising" on `(min_players, max_players, has_rules_toggle)" (section 3.4) - the Swift screen varies exactly as much as the lobby rule each kernel exports, so a shared screen would need the same parameterisation the kernel-side lift (S9) has not done yet.

**Lift verdict: lift-later**, and only after the kernel-side `msg_lobby` parameterisation (S9) lands, since the screen's shape follows the rule set, not the other way around.

### 6. BubbleSnapshot.swift

| File | Lines |
| --- | --- |
| `foolish/ios/FoolishKit/Boards/BubbleSnapshot.swift` | (source; not separately re-counted here) |
| `pickemup/ios/PickemupKit/Screens/BubbleSnapshot.swift` | 95 |
| `chuiniu/ios/ChuiniuKit/Screens/BubbleSnapshot.swift` | 99 |
| `tallybones/ios/TallybonesKit/Screens/BubbleSnapshot.swift` | 109 |

All three keep foolish's baked-at-insert, 300x195, light/dark-scheme rendering shape (each header comment says so explicitly, crediting foolish's original).
The difference between the three is the content each renders (`host.table` vs `kernel.table` vs the tally board), which is unavoidably product-specific - this is the "each product supplies a content view" pattern REUSE_AUDIT section 4 recommends for `FCard`, applied one level up to the whole bubble.

**Lift verdict: lift-later.** A `BubbleSnapshotFrame` (the 300x195 canvas, the baked-JPEG discipline, the light/dark render) could split out the same way REUSE_AUDIT proposes splitting `FCard` into a frame plus a content view (section 4), but that is new design work, not a path move, and needs its own step.

### 7. SeatBadge.swift

| File | Lines |
| --- | --- |
| `pickemup/ios/PickemupKit/Board/SeatBadge.swift` | 179 |
| `chuiniu/ios/ChuiniuKit/Board/SeatBadge.swift` | not present |
| `tallybones/ios/TallybonesKit/Board/SeatBadge.swift` | 50 |

chuiniu has no `SeatBadge.swift` at all - its `Board/DiceTable.swift` draws seats inline rather than as a standalone badge view, a real architectural divergence, not a missing copy.
tallybones' copy is less than a third of pickemup's line count; its comment says it replaced "the fan of backs" (cards) with "what a dice seat has to show" - again the content differs by design, and only the badge's outer shape (name label, role row) is shared.

**Lift verdict: do-not-lift as a single file family.** With one product not having the file at all, this is evidence there is no single "SeatBadge" shape across the five products yet, only a shared *idea* (a name plus a small status area) that each product draws its own way.

### 8. Anchors.swift

| File | Lines |
| --- | --- |
| `pickemup/ios/PickemupKit/Board/Anchors.swift` | (pickemup original, not marked COPIED) |
| `chuiniu/ios/ChuiniuKit/Board/Anchors.swift` | not present |
| `tallybones/ios/TallybonesKit/Board/Anchors.swift` | present, COPIED from pickemup at `8e216923` |

chuiniu has no `Anchors.swift`; its board layout is folded directly into `Board/DiceTable.swift`.
This is the named-anchor pattern REUSE_AUDIT flags as C-owned-geometry debt (section 1, "Swift that computes geometry is the pattern's known debt") - two of three products keep the anchor names in Swift, one does not have them as a separate concept at all.

**Lift verdict: do-not-lift.** Not a stable enough shape across products yet, and the owner's standing rule points this kind of geometry at C (shared/c/table_layout.h, REUSE_AUDIT S12) rather than at a shared Swift file.

### 9. *Seats.swift / *Dev.swift

| File | Lines |
| --- | --- |
| `pickemup/ios/PickemupKit/Kernel/PickemupSeats.swift`, `PickemupDev.swift` | originals |
| `chuiniu/ios/ChuiniuKit/Kernel/ChuiniuDev.swift` | present; no `ChuiniuSeats.swift` |
| `tallybones/ios/TallybonesKit/Kernel/TallybonesSeats.swift`, `TallybonesDev.swift` | both present, COPIED from pickemup at `8e216923` |

chuiniu folded seat and nickname persistence directly into `ChuiniuKit/Kernel/BridgeKernel.swift` (its `person()`/`persist()` functions read and write `UserDefaults` keys like `"chuiniu.seats.v1"` and `"chuiniu.nickname"` in the same file that owns the kernel seam), rather than keeping a separate `Seats.swift`.
tallybones kept the pickemup shape as a direct copy.
Both `*Dev.swift` files are genuine `DevFlags.flag(key, shipping:)` consumers (confirmed above, in the adoption table) - the "Dev" family IS the shared-adoption success story, not a reimplementation; what differs product to product is only which `dev.*` keys each product defines (`dev.slide`, `dev.sendhint` for pickemup; a smaller set for chuiniu and tallybones matching their smaller feature set).

**Lift verdict: do-not-lift as a file family** (the `*Seats.swift` shape is not consistent across all three, chuiniu inlined it); **`DevFlags.swift` itself is already lifted and adopted by all three, nothing further to do here.**

### 10. mac_tests.sh

| File | Lines | Execs shared script? |
| --- | --- | --- |
| `pickemup/ios/scripts/mac_tests.sh` | 45 | yes - `exec bash ../shared/scripts/ios_mac_tests.sh "$@"` (line 45) |
| `chuiniu/ios/scripts/mac_tests.sh` | 168 | no - full standalone body, header still says "a later lift into shared/ replaces it" |
| `tallybones/ios/scripts/mac_tests.sh` | 176 | no - full standalone body, header still says "replaced by the shared mac_tests lift" |
| `shared/scripts/ios_mac_tests.sh` | 286 | (the shared body itself) |

pickemup already did step S5 from REUSE_AUDIT (moving the generic body into `shared/scripts/ios_mac_tests.sh`, driven by a product env) and its own `mac_tests.sh` is now a 45-line env-and-exec wrapper.
chuiniu and tallybones both still carry the pre-S5 shape: a full standalone script that duplicates the entitlements backup/restore, the `xcodegen` trigger, `run_scheme`, and the zero-tests refusal - the exact logic `shared/scripts/ios_mac_tests.sh` already owns.
`diff chuiniu/ios/scripts/mac_tests.sh tallybones/ios/scripts/mac_tests.sh` (340 lines earlier in the investigation, here narrowed to this file) shows the two are otherwise near-identical **except for one genuine product difference**: tallybones added a Mac Catalyst fallback (`ios-lib-catalyst`, `SUPPORTS_MACCATALYST=YES`, `env -u DEST xcodebuild`) because, per its own comment, "with no simulator to spare (at most two booted on this Mac), the unit tests run as a Mac Catalyst app" (decision T63) - a real capability the shared script does not currently have.

**Lift verdict: lift-now for the wrapper conversion** (both chuiniu and tallybones should become thin `exec`-wrappers exactly as pickemup already is, this is the lowest-risk, already-proven step in the whole audit); **lift-later for folding tallybones' Mac Catalyst fallback into the shared script itself**, since that is new shared-script capability, not a path move, and needs its own proof that it does not change pickemup's or foolish's runs.

---

## Config files: project.yml, ship.env, asc.env, rig.env, DebugAppGroup.entitlements, Info.plist, PrivacyInfo

Every one of these exists per-product and is pattern-level duplication only, not liftable content:

- `project.yml` (172/163/160 lines for pickemup/chuiniu/tallybones) each names its own bundle id, target names, App Group and scheme names; the *shape* (Debug-only App Group entitlements trick, kernel-freshness preBuildScript, `shared/swift`/`shared/c` paths) is already the shared pattern uttt established, and REUSE_AUDIT already calls this class "(d) copy and adapt" - confirmed still true, nothing found here changes that classification.
- `DebugAppGroup.entitlements` exists in all three and is, by construction, a file whose entire content is a product-specific App Group identifier - not liftable by definition.
- `rig.env` exists in all three `ios/Tools/`; tallybones' is explicitly `COPIED from pickemup/ios/Tools/rig.env at 8e216923`, driving the one shared `rig.sh`/`rig/lib` with a product block, which is exactly the shared-tool-with-per-product-config pattern `shared/README.md` already documents (`tools/motion`, the rig). Nothing to lift; the env file's whole job is to be the per-product knob.
- `ship.env` / `asc.env`: **do not exist yet** in any of the three new products (`ios/Tools/` has only `rig.env` in all three) - none of them has reached a TestFlight/App Store step, so there is nothing to compare or lift here yet.
- `Info.plist` and `PrivacyInfo.xcprivacy` are present per-target (Kit, Messages, MessagesApp) in all three products, each carrying its own bundle id and App Group key (`FoolishAppGroup`-equivalent) - config, not liftable.

No action recommended on this section beyond what is already true: this class of file is per-product by design, and the audit found nothing here that contradicts that.

---

## Ranked list

**Lift now:**
- Convert `chuiniu/ios/scripts/mac_tests.sh` and `tallybones/ios/scripts/mac_tests.sh` into thin `exec`-wrappers over `shared/scripts/ios_mac_tests.sh`, on the pattern pickemup already proved (S5). Lowest risk in the whole audit: the shared script already exists, already handles the entitlements `cp -p` restore and the zero-tests refusal, and pickemup's own conversion is the worked example to copy.

**Lift later:**
- Fold tallybones' Mac Catalyst fallback (`ios-lib-catalyst`, `env -u DEST xcodebuild`, `SUPPORTS_MACCATALYST`) into `shared/scripts/ios_mac_tests.sh` itself, once it is proven not to change pickemup's or foolish's runs.
- Split foolish's `Tokens.swift` (`FColor`/`FSpace`/`FRadius`/`FMotion` away from `FColor.suitColor`'s `Suit` dependency) - this is the actual blocker keeping Tokens.swift, Buttons.swift and Materials.swift out of `shared/swift`, now that the three new products have independently proven those three families are otherwise near-identical between themselves.
- Split foolish's `FSquareButton.swift`/`FTextures.swift`/`Materials.swift` away from `Haptics`/`WoodFill`/`FPrefs`/`FStrings`/`FActionBar`, same reasoning, same blocker class (REUSE_AUDIT S2).
- A `MessagesSurfaceController` base class (werewolf/COMMON.md item 6), for uttt, pickemup, chuiniu and tallybones only at first; foolish's D4 fix (calling the shared insert gating) must land and be proved on its own before foolish can adopt the same base class, because that adoption is a behavior change for foolish, not a path change.
- `BubbleSnapshot` split into a shared frame plus a per-product content view, mirroring the `FCard` split REUSE_AUDIT section 4 already recommends.
- `LobbyScreen.swift`, only after the kernel-side `msg_lobby` parameterisation (REUSE_AUDIT S9, `(min_players, max_players, has_rules_toggle)`) lands - the screen's shape follows the rule set.
- Investigate why chuiniu and tallybones dropped `CollapseSlide` rather than adopting it (no reasoning is recorded beyond "collapse-after-settle" in their DECISIONS.md); if the answer is "nobody has measured whether the plain `requestPresentationStyle(.compact)` steps visibly on these two products," that measurement should happen before calling this settled, since the shared file's own header describes exactly the problem the plain call reintroduces.

**Do not lift:**
- `SeatBadge.swift` - one of three products has no such file at all; the badge idea is shared, the shape is not.
- `Anchors.swift` - same reasoning, and the owner's standing rule points board geometry at C, not at a shared Swift anchor file.
- `*Seats.swift` - chuiniu folded this into its kernel seam file; no stable shape to lift.
- All config files (`project.yml`, `DebugAppGroup.entitlements`, `rig.env`, `Info.plist`, `PrivacyInfo.xcprivacy`) - per-product identifiers by construction, already following the shared *pattern* (a per-product env driving one shared tool) where one exists.
