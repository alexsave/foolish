# Next steps — kernel consolidation · iOS MVP · iMessage (2026-07-15)

*Status snapshot + work order, written from the head of
`claude/ios-design-combined` (`8782e2f`). Everything below was verified
against that commit, not summarized from memory: `make tests` (403/403),
`make ios-smoke` (green, incl. the 48-game v6 share sweep), the `ios` CI
workflow (green on the branch head), and the three new e2e parity suites
run locally (`bot_roster_parity` + `bot_drive_parity` + `replay_v6_parity`,
18/18 pass). Known pre-existing failure, NOT from this branch:
`make difftests` dies at `solver_difftest np=2` with byte-identical numbers
on `main` (mismatches=428) — housekeeping, tracked in §5.*

*The three goals, in the owner's words: (1) move a bunch of stuff to the C
kernel, (2) get an MVP iOS app, (3) build the iMessage app. Some of each is
already done; this doc says exactly which parts, and what to do next in what
order.*

---

## 0. Where the branch stands, in one table

| Goal | Done on this branch | Remaining | Governing doc |
|---|---|---|---|
| 1 · C kernel | **A1–A4 landed and parity-proven**: one bot roster+knobs table (`bot_roster.c`/`bot_knobs.c`), one bot drive cycle + pacing table (`bot_drive.c`) on BOTH hosts, kernel-emitted animation events for local play (`evwire_walk`, `BoardDiff.swift` cancelled), v6 replay produced by the kernel on both hosts (`replay_encode_v6_from_game`) with `finalizeEndedGame` reduced to call-verify-store. Three live behavior divergences fixed on the way. | A1 server cutover · A5 · A6 · A7 · A8 (§1) | `C_CORE_CONSOLIDATION.md` §6 |
| 2 · iOS MVP | Milestones A–E **written**; C bridge proven on Linux; online wire resolved and source-verified; app build repaired + goldens regenerated on this branch. Design studies merged: SE 8-player layout, bot→Russian-cities naming, consolidated from two parallel passes. | Reconcile PR #93 · first Mac session · animation + replay-playback rendering · Milestone F (§2) | `IOS_APP_DESIGN.md` §17, `ios/README.md` |
| 3 · iMessage | **Design 100%, code 0%.** Protocol spec, mockups, `/m/` fallback, and a zero-context work order with verified spec corrections (32-byte seed, seat-prefixed awire actions, format-6 reality check). | All of M0–M5; M0 needs no Mac (§3) | `IMESSAGE_IMPLEMENTATION_HANDOFF.md` |

Branch facts: 23 commits ahead of `main`, `main` has not moved (fast-forward
merge possible). It contains both parallel iOS design passes
(`ios-app-design-98t5ap`, `ios-app-design-architecture-2aen7t`) and the
bot-localization branch (= open PR #99, so #99 is redundant once this
merges). It does NOT contain `claude/ios-redesign` (= open PR #93) — see
§2.1, that reconciliation is the biggest open decision.

---

## 1. Goal 1 — finish the C consolidation (A5–A8 + one cutover)

A1–A4 were the high-leverage items and they are done. What remains, in
recommended order (details + verification recipes in
`C_CORE_CONSOLIDATION.md` §6):

1. **A1 server-side cutover** — delete the TS `env` blocks in
   `bot_strategy.ts` and fold `wasm_choose_move`'s strategy switch onto the
   roster table. Deliberately one change: linking `bot_roster.c` into
   bots.wasm drags in two offline-only brains, so it is the same change that
   rewrites `bots.wasm.gz`. Until then the server agrees with the kernel by
   construction (identical env table), so this is safe to schedule, not
   urgent.
2. **A5 — replay steps from the kernel (F4.2).** Reuses the A3 emitter.
   Do this NEXT among the kernel items because it directly unblocks the two
   iOS rendering gaps (§2.3): native replay board-playback and the web's
   ~800-line TS replay twin both become decode-and-render.
3. **A6 — reset-to-lobby transform (F6), then the F8 projection deletions.**
   Cleanup wave; rematch e2e on web + iOS is the gate.
4. **A7 — F9 batch, opportunistic.** Note two riders already spoken for:
   the seed encode/decode rejects ride iMessage M0 (§3), and
   `unambiguous_cover` rides the first surface that wants one-tap cover
   (phone tap-commit or iMessage). The `console`+`gpt` strategy deletion can
   go any time.
5. **A8 — web wire-decode folds (F7),** format-by-format on next wire
   change. Watch the guards.wasm memory budget note in §4.7.

## 2. Goal 2 — get the iOS MVP out

### 2.1 Decision first: reconcile PR #93 (`claude/ios-redesign`)

PR #93 is 18 commits of **Mac-verified** work (builds green, 20 unit/snapshot
tests + a drag-to-play UI test) that this branch does not contain: procedural
wool/wood/fern textures in Swift, drag-to-play, the 8-seat arc layout, the
app icon, and an `ios_goldens.c` buffer fix. It is based on an older `main`
and a test-merge against this branch conflicts in exactly six files:

- `ios/FoolishKit/Engine/LocalGame.swift`, `Models.swift` — the real ones:
  this branch rewired both onto `fio_bot_drive_json` + kernel events; #93
  reworked the same files for UI. Resolution rule: **this branch's engine
  wiring wins, #93's UI/texture/layout work wins**; re-run goldens + Swift
  tests after.
- `cnitro/ios/ios_goldens.c` — both changed it; merge both (buffer fix +
  this branch's regeneration path), then `make ios-goldens` and commit.
- `ios/project.yml` — additive on both sides; merge both.
- `docs/WATCHOS_LAYOUT.md`, `docs/watchos-layout.html` — add/add; keep the
  merged #96/#98 versions already on `main`/this branch, discard #93's copy.

The design studies on this branch already agree with #93's direction —
`IOS_PHONE_LAYOUT.md` reverses the flat-felt decision in favor of the
website's wool/wood/fern materials, which #93 implements — so this is a
merge, not a fork. **Recommended order: land this branch on `main` first
(it is a fast-forward), then rebase/merge #93 onto it,** because #93's
Mac-session artifacts (snapshot references, icon) are downstream of the
engine files this branch rewrote.

### 2.2 The single blocking step: a Mac session

Everything Swift on this branch is written-to-compile but has never seen
`xcodebuild` (PR #93 HAS built on a Mac — one more reason to reconcile it
first and inherit its fixes). The checklist is `IOS_APP_DESIGN.md` §17.6,
unchanged: `make ios-lib` → `xcodegen generate` → `xcodebuild build test` →
record snapshot references → staging-Supabase end-to-end run (the wire is
resolved; this is runtime verification) → `xcconfig` keys. Milestone F
(signing, TestFlight, submission) additionally needs the Apple Developer
account. PR #93 notes staging credentials were the blocker for the online
run — provisioning a staging Supabase project is a prerequisite worth doing
before the Mac day.

### 2.3 Rendering work unblocked by the kernel items

- **Card-flight animation (Milestone B remainder)** — A3 landed the kernel
  event stream into Swift (`GameEvent`/`lastEvents`); what remains is
  consuming it in `TableView` (matched-geometry flights). No Swift diff
  engine — `BoardDiff.swift` stays cancelled.
- **Replay board playback (Milestone C remainder)** — render decoded
  replays step-by-step on the full board. Do after A5 so the steps come
  from the kernel rather than a second projection.
- Small: camera QR scan, full a11y pass, `action_goldens.json` parity
  fixture (§16.D3).

### 2.4 MVP scope call (recommendation)

Ship v1 = offline + replays + online (Milestones A–F as specced), with
2.3's animation polish if the Mac session goes smoothly. Watch stays parked
per §17.9. iMessage ships separately after M5 — do not gate the app on it.

## 3. Goal 3 — start the iMessage app (M0 is Linux-only)

Nothing is built; the work order is `IMESSAGE_IMPLEMENTATION_HANDOFF.md` §4
and it was deliberately structured so **M0 (5–8d) runs entirely in this
repo's existing Linux toolchains** — it can start now, in parallel with the
Mac work, and it is mostly Goal-1-style C work:

- `cnitro/src/msg_wire.{h,c}` (style-match `awire.c`; SHA-256 for `parent8`
  + Rule P tiebreak) + `msg_wire_test.c` in `make difftests`.
- `wasm_msg_encode/decode` exports + TS bridge (`kernelMsgEncode/Decode`).
- `e2e/msg_wire.test.ts` + `e2e/msg_concurrency.test.ts` — Rule P/Rule R as
  pure TS over the wasm exports, property-tested; these become the fixture
  oracle the Swift port must match in M3.
- `/m/[payload]` read-only route with install/play CTAs.
- Rider from A7: the seed-length encode/decode rejects land here.

M1–M5 (Xcode target, extension UI, Messages wiring + concurrency layer,
game end + l10n, review prep) queue behind the first Mac session and the
host app, per the handoff.

## 4. Suggested sequencing (one plausible fortnight)

1. **Open the PR for `claude/ios-design-combined` → `main` and merge it.**
   `main` hasn't moved, `ios` CI is green on the head, and opening the PR
   makes the `validate` e2e suite (which only fires on PRs to `main`) run
   the three new parity suites in CI — they pass locally. Then close #99
   (contained) and update its branch note in `IOS_APP_DESIGN.md` §17.1.
2. **Rebase/merge PR #93 onto the new `main`** per §2.1's resolution rule;
   regenerate goldens; get its Mac checks green again.
3. **Start iMessage M0 on Linux** (§3) — parallel track, no Mac needed.
4. **Kernel: A5** (replay steps), then queue the A1 server cutover with the
   next bots.wasm rebuild.
5. **Mac session** (§2.2): build+test the merged app, snapshots, staging
   e2e; then Milestone F prep as account/credentials allow.

## 5. Housekeeping (not gating anything)

- `solver_difftest` fails identically on `main` and this branch
  (np=2: compared=486 mismatches=428) — `make difftests` therefore never
  reaches `replay_difftest`/`replay_v6_test` locally even though
  `replay_v6_test` passes when run directly. Diagnose or quarantine so the
  target is trustworthy again.
- `game_snapshots.extras` replay-name blob is still not anonymized on
  account deletion (documented in the migration) — needs replay
  re-encoding; revisit before Milestone F relies on the deletion story.
- Stale branches: the two design-pass branches and `savepoint/*` can be
  deleted once this branch merges.
