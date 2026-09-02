# Pre-Mortem — Shipping the Foolish iMessage Extension to the App Store

## Session Details

| Field | Value |
|-------|-------|
| **Product / Feature** | Foolish — Durak, standalone iMessage App Store record (`cards.foolish.msg`, `FoolishMessages.appex`) |
| **Planned Launch** | Next App Store submission |
| **Pre-Mortem Date** | 2026-07-25 |
| **Facilitator** | Adversarial pre-mortem pass (this document) |
| **Grounding inputs** | `docs/APP_REVIEW_NOTES_2026-07-25.md` (fresh hands-on review, Report 3) + code investigation of the deal path, the message-bubble protocol, and abandonment handling |

> **The thought experiment:** It is some weeks after launch and the ship has failed — either App Store review rejected it, or it was approved and is a bad experience for real people in real iMessage threads. Working backward, here is why.

---

## The one clarification that reshapes everything below

There are **two separate online architectures** in this repo, and the brief conflates them. The thing being submitted is the **standalone iMessage extension**, which is *not* the Supabase/bots product.

| | **iMessage extension** (this submission) | **Host app + web** (deferred, NOT this submission) |
|---|---|---|
| Transport | Apple iMessage bubbles (`MSMessage`) | Supabase realtime + edge functions |
| Network stack | none — links `FoolishKit` only | `ios/FoolishNet/*` |
| Bots | **none** — solo seats are `#if DEBUG \|\| SOLO_TESTING` | server-side `bot_drive` loop |
| Concurrency model | Rule P / Rule R (git-style rebase of moves) | CAS on `games.version` + `round_epoch` fence |
| Abandonment recovery | none by design (correspondence-chess) | none either (see E-cluster) |

Evidence: `ios/project.yml` (`FoolishMessages` depends on `FoolishKit` only), `docs/IMESSAGE_APP_STORE_SUBMISSION.md` ("zero network stack… entirely peer-to-peer through the message chain"), `docs/IMESSAGE_GAME_DESIGN.md` ("iMessage v1 ships rules only, no bots"). A Release build of the extension has **no bots and no server**.

**Consequences for this pre-mortem:**
- The scary Supabase failure modes (cold-start latency, dropped realtime push, the old bot-loop freeze) belong to a product that is **not shipping in this submission** → Paper Tigers here.
- The reviewer's 6-player table was populated via the **DEBUG-only** `addSoloSeat` path. In a real thread a 6p game needs six real humans — rarer — but the compact-clipping and duplicate-render defects it surfaced live in **shipped** code and reproduce with real joins.
- "A human abandons mid-bout" is not a crash — it is an unanswered text. With no bots and no server, a half-finished game just dies in the thread. That is the dominant *real-user* failure mode, and it is wide open.

---

## Summary

| Metric | Count |
|--------|:-----:|
| **Total Risks Identified** | 20 |
| **Tigers** | 10 |
| -- Launch-Blocking | 3 |
| -- Fast-Follow | 4 |
| -- Track | 3 |
| **Paper Tigers** | 6 |
| **Elephants** | 4 |

---

## Ranked failure modes (likelihood x impact)

Ordered most-to-least dangerous. L = likelihood, I = impact.

| Rank | # | Failure mode | L | I | Class / Urgency |
|---|---|---|---|---|---|
| 1 | E1 | No human has ever completed a full game over real transport; the core loop (attack/cover/pass/take, bout-end sweep, game-over, leaderboard) is unverified end to end | High | Critical | Elephant → Launch-Blocking gate |
| 2 | T1 | Compact clips the primary "Start playing" CTA (and a seat); expand grabber is unlabeled; lobby doesn't scroll → functional dead-end + Apple rejection | High | High | Tiger / Launch-Blocking |
| 3 | T2 | Visible duplicate card on the play surface (animation/identity double-render, not kernel state) → reads as a broken/cheating card game | Med | High | Tiger / Launch-Blocking |
| 4 | T3 | Hot-pink/magenta wool + low-contrast overlaid labels → the app "reads unfinished"; 1-star bait, possible 4.0 design rejection | High | Med-High | Tiger / Launch-Blocking |
| 5 | T5 | Mid-bout abandonment silently kills the game — no forfeit/timeout/nudge; guaranteed in casual threads | High | High | Tiger / Fast-Follow (design) |
| 6 | T4 | Turn/role shown only by tiny low-contrast sword/shield icons; no "Your turn"/"Waiting" text → players can't tell whose move it is | High | Med | Tiger / Fast-Follow |
| 7 | T6 | Card ranks/pips ignore Dynamic Type — the game's core info can't be enlarged; flagged in TWO prior reviews | Med | Med | Tiger / Fast-Follow |
| 8 | T7 | Hand cards under the home-indicator safe area in compact; sub-44pt tap targets | Med | Med | Tiger / Fast-Follow |
| 9 | E2 | Recurring never-closed blockers across THREE review passes (esp. leaderboard-off-screen) — fixes re-noted, not landed | Med | Med | Elephant |
| 10 | T8 | Leaderboard "walks off screen" still unverified (never reached in review) | Med | Med | Tiger / Track |
| 11 | E4 | Deadline pressure waives the two must-verify items (duplicate-card repro, live full-game proof) | Med | High | Elephant |
| 12 | E3 | Store metadata/screenshots describe the wrong product (online/bots) vs the peer-to-peer human-only binary actually shipping | Med | Med | Elephant |
| 13 | T9 | iMessage app-list icon reads as a dark muddy blob at list size | Med | Low | Tiger / Track |
| 14 | T10 | Nickname-too-long gives no target length and doesn't cap input | Med | Low | Tiger / Track |

Paper Tigers (P1-P6) sit below the line — see their section for why each looks scary but isn't in scope or is already fixed.

---

## Tigers (Real Risks)

### Launch-Blocking

| # | Risk | Evidence | Mitigation | Owner | Decision Date |
|---|------|----------|-----------|-------|--------------|
| T1 | Compact clips the primary "Start playing" CTA and one opponent seat; the drag-to-expand grabber is unlabeled and the lobby doesn't scroll. iMessage apps open compact-first, so a user can hit a dead-end where the primary action is unreachable. | Review finding 2 (screens 04/05/06/07). Apple routinely rejects flows where a primary action is off-screen (Guideline 2.1 / 4). | Make the compact lobby scroll to the CTA, OR keep "Start playing" pinned in the compact safe area, OR auto-request `.expanded` when a lobby needs it. Add a visible label/hint on the grabber. Re-verify in the real extension, not just the harness. | Alex | Before submission |
| T2 | A duplicate playing card was visible on the primary play surface (trump 9♠ under the deck also in hand). Kernel state cannot dupe (deal draws the flip out of the deck; harness == production deal), so this is an **animation/identity double-render** in shipped board code (`MessageTableView`/`BoardFlight`, the documented bug-9/10 family) or the late-game stale-`g->flipped` path — both ship. A visible duplicate reads as a broken/cheating card game regardless of root cause. | Review finding 1 (screens 08a/08b/07). Deal path proven single-primitive (`game_seat_and_deal` → `deal_shuffle`). Double-render history in `MessageTableView.swift` L190-210, `BoardFlight.swift` L154-205; stale-flip landmine in `view.c` L19-88. | Reproduce on a **real dealt game** (real joins, not `addSoloSeat`). If it repros: fix the `matchedGeometryEffect` identity/veil collision in `BoardFlight`; add a `card.identity`-uniqueness assert on the rendered view. If it does not repro, capture proof and record it. Do NOT ship on the assumption it is harness-only. | Alex | Before submission |
| T3 | The hot-pink/magenta wool background is garish, fights the fern card backs and red pips, and drops the contrast of white seat labels and grey turn markers. This is the single biggest reason the product "reads unfinished." | Review finding 3 (every screen; confirmed in the real extension, screen 16). Reputation + soft Guideline 4.0 (design) risk + contrast concern for overlaid UI. | Retone the wool to a calmer felt/green or muted neutral; add a subtle scrim behind overlaid labels/markers to guarantee contrast. Cheap change, highest perceived-quality return. | Alex | Before submission |

### Fast-Follow (within 2 weeks post-launch)

| # | Risk | Evidence | Plan | Owner | Target |
|---|------|----------|------|-------|--------|
| T4 | Turn and attacker/defender role are conveyed only by tiny low-contrast sword/shield icons; no "Your turn" / "Waiting for Solo 2…" text. New players cannot tell whose move it is (reviewer repeatedly guessed and got illegal-move toasts). | Review finding 4 (screen 12). | Add explicit "Your turn" / "Waiting for X" text + a seat highlight. Pairs with T3 (contrast). Strongly consider pulling into the launch-blocking set — turn-taking legibility is the whole point of a turn game. | Alex | +1 week |
| T5 | A human who closes Messages mid-bout leaves the game permanently unadvanceable. No bots, no server, no forfeit/timeout/nudge — the game silently dies in the thread. In a casual iMessage audience this happens constantly. | Extension has no bots/server (design). No abandon/timeout handling anywhere (grep-confirmed). Host/web path is no better: `handleExit` refuses mid-game exit, no seat ever flips to AI, heartbeat pokes futilely for 1h then drops the game. | Design a graceful abandonment story: a "nudge/remind" affordance, a "this game went quiet" state, and/or an agreed forfeit after N days. This is a product-design task, not a bug fix; it is the biggest liveness risk to real-thread enjoyment. | Alex | +2 weeks |
| T6 | Card ranks/pips are fixed-size graphics that do not respond to Dynamic Type. The most important information in a card game is exactly what a low-vision user cannot enlarge. Named in TWO prior review passes. | Review finding 6 (screens 09/10). | Make card rank/suit scale with the content-size category (or add a "large cards" toggle). Accessibility focus area for Apple; recurring flag raises rejection odds. | Alex | +2 weeks |
| T7 | In compact, the hand row is pinned to the very bottom; card tap targets run under the home indicator and rounded corners, squeezing hit area below 44pt and clipping bottom pips. | Review finding 5 (screen 03). | Respect the bottom safe-area inset; lift the hand row; ensure >=44pt targets. | Alex | +2 weeks |

### Track (monitor, act if it escalates)

| # | Risk | Monitoring signal | Tripwire |
|---|------|-------------------|----------|
| T8 | Leaderboard "walks off screen" (prior blocker) is still **unverified** — the review never reached a completed game. | First real end-to-end game (see E1). | If the score/leaderboard screen clips at any Dynamic Type size → promote to Launch-Blocking. |
| T9 | iMessage app-list icon is a dark maroon blob; jester detail is illegible at list size next to Apple's brighter icons. | User confusion / low tap-through in the "+" drawer. | Brighten/simplify the list-row glyph if install funnel underperforms. |
| T10 | "nickname too long" disables the button but doesn't cap input or state the max — guess-and-check. | Support/feedback about naming. | Add a hard character cap + a live counter. |

---

## Paper Tigers (look scary, unlikely or out of scope)

| # | Risk | Why it is a Paper Tiger | Would become a Tiger if… |
|---|------|-------------------------|--------------------------|
| P1 | "The deal produced a genuinely duplicated card (state corruption) that will corrupt legal-move logic." | The C deal draws the flipped trump *out of* the deck via `draw_card` and never places it in a hand; the 52-card 6p deal leaves 16 cards after 36 dealt. Harness and production share the exact same `game_seat_and_deal` primitive — there is no separate harness deal to blame or trust. The *visible* duplicate is a render issue (T2), not state. | A repro shows two identical `card.identity` values in the **kernel view** (not just on screen). |
| P2 | Supabase netcode failure modes — cold-start latency on first move, dropped realtime push, resync-on-foreground not wired. | These live in `FoolishNet` / the edge functions, which the **iMessage extension does not link**. Not part of this submission. (They are real for the deferred host app and tracked there.) | The host app is added to this submission, or the extension gains a network path. |
| P3 | The bot-loop freeze (`EdgeRuntime.waitUntil` baton leak) and the optimistic-revert flicker. | Both **fixed** with durable mechanisms — auto-expiring bot lease (`BOT_LEASE_TTL_MS`), `round_epoch` fence + `REJECT_STALE_ROUND` — plus regression tests. And both are host/web only; the extension has neither bots nor optimistic online mutation. | A regression test is deleted or the guard is removed. |
| P4 | Crashes / hangs / rotation / rapid-tap / long-nickname instability. | The review exercised all of these adversarially and saw **no crashes, hangs, or beach-balls**. Robustness is a current strength. | A new code path (e.g. the T2 render fix) reintroduces a hang. |
| P5 | Landscape layout is broken (large empty margins). | iMessage runs portrait; landscape is irrelevant to the extension. | N/A. |
| P6 | armv7 capability / missing privacy manifest / entitlements rejection (prior blockers). | Verified resolved: the `UIRequiredDeviceCapabilities=[armv7]` entry is dropped (Info.plist comment), `PrivacyInfo.xcprivacy` ships for the extension and FoolishKit, and the App Group entitlement is present and regenerated by xcodegen. | A build-config regression re-adds armv7 or blanks the manifest. |

---

## Elephants (the things nobody quite says out loud)

| # | Elephant | Assessment | Resolution |
|---|----------|-----------|-----------|
| E1 | **No one has ever played a full game to completion over real transport.** The review could not test a single bout: harness Solo bots never take their turn, named identities each get an *independent* game, and synthetic taps don't register as card plays. So cover/take/pass, the bout-end sweep, game-over, win/loss, and the leaderboard are all unverified on device. We would be shipping a card game where a complete game has never been observed to work over iMessage. | This is a Tiger wearing an Elephant costume — the biggest single ship risk, and it is comfortable to skip because it is hard to test. | **Launch-Blocking gate:** run one real end-to-end game across two Messages participants (two simulators or a device pair per `IMESSAGE_MAC_RUNBOOK.md`) before submission. This gate also closes T8 (leaderboard). Reclassify to a hard blocker. |
| E2 | **The same blockers keep recurring across three review passes** (leaderboard-off-screen named in prior reviews, still open here). Are fixes actually landing, or being re-noted each cycle? | Process risk. Suggests the fix→verify loop isn't closing. | Maintain a single living blocker ledger keyed to the review reports; require a screenshot/test proving each closed item before the next submission. |
| E3 | **The store listing may describe the wrong product.** Docs were flagged stale on their "single most load-bearing fact" (the one-app vs two-app split reversed mid-stream). If metadata/screenshots describe online play or bots, they mismatch the peer-to-peer, human-only binary that ships — an accuracy/2.3 risk, and a promise the app can't keep. | Real, avoidable. | Audit `IMESSAGE_APP_STORE_SUBMISSION.md` metadata and every screenshot against the actual Release build: no bots, no accounts, no online, peer-to-peer only. |
| E4 | **Deadline pressure to make "the next submission" leads to waiving exactly the two must-verify items** the review flagged (duplicate-card repro, live full-game proof). | These are the two most consequential and the two easiest to skip. | Make them explicit go/no-go gates (below), not backlog items. |

---

## Action Plan — Launch-Blocking Items

### E1 (gate): Prove one full game over real transport
- **Mitigation:** Drive a complete 2-participant game (two simulators sharing a conversation, or a device pair) through attack → cover → pass/take → bout sweep → game-over → leaderboard. Fix whatever breaks; capture the leaderboard screen to close T8.
- **Fallback if not done by decision date:** No-Go. Do not submit a card game whose full loop is unproven on device.
- **Status:** Not started.

### T1: Compact CTA reachability
- **Mitigation:** Scroll-to-CTA or pinned CTA or auto-expand for lobbies; label the grabber; re-verify in the real extension.
- **Fallback:** No-Go for review (Apple rejects unreachable primary actions).
- **Status:** Not started.

### T2: Duplicate-card render
- **Mitigation:** Repro on a real dealt game; fix the `BoardFlight` identity/veil collision or prove non-repro with evidence.
- **Fallback:** If it repros and can't be fixed in time, No-Go — a visible duplicate card is fatal to a card game's credibility.
- **Status:** Not started.

### T3: "Reads unfinished" background/contrast
- **Mitigation:** Retone wool; add label/marker scrim.
- **Fallback:** Conditional-Go acceptable only if T4 turn-legibility also lands, since the two together drive the "unfinished" read.
- **Status:** Not started.

---

## Launch Decision

| Field | Value |
|-------|-------|
| **Decision** | **Conditional Go** |
| **Conditions** | (1) E1 full-game proof passes; (2) T1 compact CTA reachable and re-verified in the real extension; (3) T2 duplicate reproduced-and-fixed or proven non-repro; (4) T3 background retoned. T4 strongly recommended in the same pass. |
| **Rationale** | No crash/stability blockers — robustness is genuinely strong. The blockers are reachability, a visible correctness-*looking* defect, and finish quality, plus the untested core loop. All are addressable without deep rework, but shipping without them risks both rejection and a launch that "reads unfinished" to real users. |

---

## Follow-Up Schedule

| Action | Owner |
|--------|-------|
| E1 full-game proof + close T8 | Alex |
| Verify T1/T2/T3 fixes in the **real extension** (not harness) | Alex |
| Metadata/screenshot audit vs Release build (E3) | Alex |
| Final Go/No-Go | Alex |
| Post-launch Fast-Follow sprint: T4, T5 (abandonment design), T6, T7 | Alex |

---

## Notes

- The strongest asset going in is stability: heavy adversarial interaction produced no crashes/hangs, nickname validation is fixed, text labels honor Dynamic Type, and the illegal-move toast is clear. The failure modes are quality, reachability, and an unproven core loop — not fragility.
- The most under-appreciated risk is **T5 / abandonment**: it will not show up in a review pass, but it is what real iMessage threads will do constantly, and there is currently no design answer for it at all.
