# Foolish on the phone — iPhone SE layout study (decision record)

*July 2026. The offline-first phone table: how an 8-player Durak game fits an
iPhone SE tastefully, leaning on the website's design. Deliverables:*

- **`docs/ios-phone-layout.html`** — the canonical mockups: three options at
  native SE points inside a bezel, real-size control, thumb-reach and
  zone-map overlays, stress states, the Russia-roster picker (en + ko), and a
  tap-to-attack live demo. **Option B ("Horseshoe") is the final layout.**
- **This doc** — the constraints, the option comparison, the decision, and
  the platform research that de-risks "watch later, network later."

Relationship to `docs/IOS_APP_DESIGN.md`: that doc remains the build plan and
already prescribes the banded table (§6 row 3, §16.B3 — "opponents arced top,
battles center, deck well trailing, fan bottom"). This study **confirms that
skeleton against the SE worst case, fixes its numbers, and reverses one §5
decision**: the game screen wears the website's materials (wool table, wood
buttons, fern backs), not flat "Gosizdat" felt — see §4. The strategic
context (why offline-first now): the watch design is finished and parked
(no hardware for ~a month, `IOS_APP_DESIGN.md` §17.8); the push is
bots-on-the-phone with zero network, then iMessage.

---

## 1 · Constraints (engine-verified, same fact base as the watch study)

| # | Fact | Source | Layout consequence |
|---|---|---|---|
| 1 | 8 players ⇒ 52-card deck, 7 opponents, hands refill to 6 | `card.h`, `constants.ts` | Worst case is 7 seat chips + a big fan, simultaneously |
| 2 | No table-slot cap; post-pickup defender can face 10+ pairs | `game.c:557-559` | Battle grid must shrink → wrap → overflow-chip, never clip |
| 3 | Any non-defender may throw in at any time; no turn order to draw | `game.c:538` | No "waiting on X" arrow; defender shield is the only role mark |
| 4 | No round timer; rounds close by unanimous GOOD | `good.ts:7-8`, `game.c:840-869` | No countdown chrome anywhere |
| 5 | Flipped trump is public and drawn last | `game.c:321-333` | Deck well shows flip card, then decays to bare suit glyph |
| 6 | Legality is kernel-only (guards precedent; `fio_legal_*` on iOS) | `clientGuards.ts`, `ios_api.c` | Buttons/cards gate from the kernel; no Swift booleans |
| 7 | SE canvas 375×667 pt, 44 pt hit floor, right-thumb arc | Apple HIG | Committable things bottom/right; look-zones top |

## 2 · Options considered

- **A · "The website, verbatim"** — port the radial `PlayerRing` ellipse.
  Rejected: at 375 pt wide the ellipse drops side seats into the fan zone,
  battles compress to ~180 pt, seat names collide with the deck/discard
  wells. The ring is a desktop luxury. (Frame A1.)
- **B · "Horseshoe" — CHOSEN.** The IOS_APP_DESIGN banded layout with the
  web's materials and one new idea: opponents sit on a **flattened arc** that
  preserves seating order (left neighbor at the left edge, right neighbor at
  the right) without ever dropping below y≈150. Zones degrade independently:
  the arc zigzag-staggers at 6+ opponents, battles wrap 4-per-row and shrink
  at 9+, the fan compresses to ~21 pt exposure at 15 cards. (Frames B1–B4,
  S1–S2.)
- **C · "Strip"** — the watch's Option-H economy scaled up (flat count strip,
  huge midfield). Viable; kept as the landscape/iPad-compact direction and as
  a fallback if Dynamic-Type-XL breaks the arc chips. On SE portrait it
  wastes its won space in typical rounds and erases identity. (Frame C1.)

### 2b. The parallel study (merged 2026-07-15)

A second, independent design pass (`ios-se-layout.html`, since retired)
attacked the same brief and landed on the strip + a one-tap roster sheet,
skinned in the flat "Gosizdat" felt. Where the two passes disagreed:

| Question | This study | Parallel study | Kept |
|---|---|---|---|
| Opponent zone | horseshoe arc (seating order preserved) | flat strip + roster sheet | **Arc** — identity on-screen matters on a phone; but the **roster sheet is adopted** as the arc's tap-through detail view |
| Materials | website's wool/wood/fern | flat felt + wool tint | **Wool/wood/fern** — the owner's "lean on the website" direction |
| Big-hand handling | tighter overlap (~21 pt exposure) | **press-drag fan scrub** (touched card lifts/magnifies; release arms) | **Both** — overlap to ~15 cards, scrub as the interaction from ~12 up; keeps a 19-card hand playable with no second row |
| Cover interaction | tap-select, tap/drag commit | **arm-then-target** (tap card, tap open attack) | **Both** — arm-then-target ships as the precision path alongside drag |
| Convergences | — | zone-stack bands, kernel-only gating, count+shield chips, ko-locale proof | agreement counts as evidence: the banded skeleton is right |

## 3 · The Option B zone spec (SE reference; layout is proportional above SE)

| Zone | Frame (pt) | Content |
|---|---|---|
| Status bar | y 0–20 | System. |
| Exit | y 24–44 left | Back chevron only; board is chrome-free in play (§5.5 of the design doc). |
| **Opponent arc** | y 44–158 | ≤7 chips: mini fern-back fan + count badge + 9.5 pt name; arc position `x=24+327·t, y=152−104·sin(πt), t=(i+1)/(k+1)`; zigzag `+16pt` on odd indices when k≥6. Defender = gold heater shield; GOOD = green; thinking bot = "···"; eliminated = 30% opacity. Chips are look-zones; tap anywhere on the arc → roster sheet. |
| **Deck well** | x 8–66, y 170–268 | Web-verbatim: rotated fern stack + count badge, flipped trump tucked beneath at 90°; bare suit glyph once drawn (fact #5). |
| **Discard** | x 312–370, y 170–260 | Fanned backs + count. Counts only. |
| **Battles** | x 70–305, y 176–436 | Pairs 56×76, covers rotated +12°, 4/row, 3 rows; at 9+ pairs drop to 44×61; at 13+ show "+N ▾" chip → sheet. Drop-target rings amber (`--color-warning`). In-flight cards 45% opacity. |
| Status line | y 442–458 | One transient toast; never a banner. |
| **You-defend mark** | y 472 left | Gold shield + "you defend" beside the action bar when you are the defender (replaces the web's center arrow — clutter at SE size; watch-proven idiom). |
| **Action bar** | y 462–506 right | Wooden 72×40 buttons, ≤3, kernel-gated, contextual from {Attack · Cover · Pass · Pickup · Good}. |
| **Hand fan** | y 510–650 | 52×73 cards, ±3°/card fan; compresses, never paginates. Tap = select (red ring + 14 pt lift), tap again / drag up = commit; illegal = shake + `.rigid`, zero dialogs. |

Interaction and animation contracts are unchanged from
`IOS_APP_DESIGN.md` §16.B3–B4 (single spring, diff-driven `matchedGeometryEffect`;
see the consolidation report for where the diff engine should ultimately live).

## 4 · The materials decision (the one deviation from IOS_APP_DESIGN §5)

The owner's direction for this phase: the phone app should **lean on what the
website does** — wool/wood/fern — rather than the doc's flat "Gosizdat" felt.
Concretely:

- **Table background = procedural wool** (port `WoolBackground.tsx`'s
  generator to CGContext, drawn once per seed and cached — the identical
  pattern already used by `FernCardBack.swift`).
- **Buttons = procedural wood** (port `WoodTexture.tsx` the same way).
- **Card backs = fern** (already ported).
- Everything else in the §5 design system stands (spacing, type, haptics,
  spring, SF Compressed numerals).

Until the two generators land, `FColor.table` + vignette remains the interim
skin — but the wool is the brand; schedule it inside the offline milestone,
not after.

## 5 · Research: is it safe to defer the watch app and networking? (Yes.)

Verified July 2026 (Apple docs unless noted; full citations in the PR that
added this doc):

1. **Adding watchOS later is routine and first-class.** Xcode's "Watch App
   for Existing iOS App" target flow; iOS + watchOS upload together from the
   same project/app record (no separate platform record; App Store Connect's
   add-platform page covers macOS/tvOS/visionOS only). Normal review, no
   penalty. Gotchas to plan for, none blocking: watch bundle id must be
   prefixed by the iOS app's (`cards.foolish.app.watchkitapp`), watch
   version/build must match the iOS app's at upload (CI pitfall), and we want
   the *companion* (not watch-only) template — which is exactly
   `WATCHOS_APP_PLAN.md`'s plan. The only repo prerequisite stays "watchOS
   slices in `make ios-lib`."
2. **Shipping offline-only v1 is unproblematic.** Nothing in the review
   guidelines requires networking; 4.2 (minimum functionality) is about
   usefulness, which offline bots + replays + tutorial clears. When accounts
   arrive later: guideline 4.8 does **not** apply to an own-account
   (username/password) system — Sign in with Apple is only forced if a
   third-party login ships; 5.1.1(v) in-app account deletion applies the
   moment accounts exist (the endpoint already merged — `4108a5e`); privacy
   labels must be updated in the same release. Adding IAP later is a normal
   update.
3. **Practical consequence for milestones:** nothing about the current
   `IOS_APP_DESIGN.md` §14 ordering needs to change; this study simply
   re-weights execution toward **A–B (offline vertical slice) as the
   shippable v1**, with D (online) deferred behind the iMessage push, and the
   watch parked exactly as §17.8 already says.

## 6 · Handoff pointers

- Components exist, uncompiled (`ios/FoolishKit/Boards/*`,
  `IOS_APP_DESIGN.md` §17.4) — this study adjusts their composition/skins,
  not their contracts. First Mac session checklist in §17.6 still governs.
- Bot names on all picker/table surfaces go through the display mapping in
  `docs/IOS_BOT_NAMING.md` (the 10-rung km-to-Moscow ladder; ⚙ icon; `%`
  never renders; the picker shows the km flavor line — mockup frames R1–R3).
- The iMessage extension reuses this exact table at 2–4 players inside its
  expanded sheet — see `docs/imessage-layout.html` M4.
- Acceptance items to add to milestone B's DoD: B1/B2/S1/S2 frames
  reproduced on device at SE size; zigzag arc verified at 8 players; card
  counts sum to 52 in every screenshot (the watch study's conservation rule).
