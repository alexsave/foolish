# iMessage app — implementation handoff (zero-context work order)

*You are an implementer (human or LLM) with NO prior knowledge of this
repository. This document tells you exactly what to build, in what order,
what already exists, and where the full specs live. Every claim about the
tree was verified 2026-07-15. The product/protocol spec is
`docs/IMESSAGE_GAME_DESIGN.md` (2026-07-13) — it remains authoritative for
everything this document does not explicitly correct in §3.*

---

## 0. Read these before writing any code

| Doc | What it gives you |
| --- | --- |
| `docs/IMESSAGE_GAME_DESIGN.md` | THE spec: product, `FMSG` payload, message protocol, identity/seats, the git-style concurrency model (Rule P / rebase), Messages API wiring, worked race examples, test plan. Read it fully. |
| `docs/imessage-layout.html` | The UI mockups (open in a browser): thread bubble (M1 faces vs M2 glyphs, light+dark), compact drawer (M3), expanded table + staged state (M4/M4b), finished card (M5), group lobby (M6), `/m/` fallback (M7) — plus the 300×195 bubble-snapshot zone spec. Build these screens. |
| `docs/IOS_APP_DESIGN.md` §17 | State of the host iOS app (built; Swift pending first `xcodebuild`). The extension ships INSIDE this app. |
| `docs/REPLAY_FORMAT6_HIDDEN_STATE.md` | The shipped v6 replay codec — what "mid-game encoding" now exists (and §3.3 below: what it does and doesn't give this project). |
| `docs/IOS_BOT_NAMING.md` | iOS bot display names (no bots in iMessage v1 — relevant only for shared strings hygiene). |
| `README.md` + `docs/ARCHITECTURE_AS_A_PATTERN.md` | The repo's one law: game rules exist ONCE, in C, compiled to wasm (server/web) and a static lib (iOS). TS/Swift marshal and render. |

**The one hard rule, repeated:** no Durak rule is ever reimplemented in Swift
or TS. Whose move, legality, capacity — always the kernel. Any hand-rolled
"is it my turn" boolean is a bug by policy (`IMESSAGE_GAME_DESIGN.md` §17.16).

---

## 1. Context in ten lines

Durak card game, 2–8 players, multi-actor (several seats may legally act at
the same moment — there is NO turn alternation; internalize
`IMESSAGE_GAME_DESIGN.md` §5.1 before anything else). One C rules kernel
(`cnitro/src/game.c`, `legal.c`, `view.c`, `replay.c`) runs the website, the
Supabase edge functions, and the native iOS app. An iMessage game has no
server: each turn is an `MSMessage` whose URL carries the ENTIRE game —
`(32-byte deal seed, ordered action list)` — and every device reconstructs
and re-validates the game by replaying those actions through the kernel.
Concurrent moves converge via a deterministic chain-preference rule plus a
rebase step (git mental model). Game over → the bubble links a standard
`foolish.cards/<code>` replay. Recipients without the app get a real web
page (`/m/<payload>`) rendering the game read-only.

---

## 2. What already EXISTS (do not rebuild any of this)

### 2.1 The C engine and its iOS bridge — done and tested

- **Kernel**: `cnitro/src/game.c`, `legal.c`, `view.c`, `replay.c`. Seeded
  deals: `game_set_deal_seed_bytes(seed, len)` (`game.h:145`) — **len must be
  ≥ 32**; the whole game is then reproducible from the seed on any platform.
- **iOS static lib + shim**: `cnitro/ios/ios_api.{h,c}`, built by
  `make ios-lib` (also `ios-smoke`, `ios-goldens`, `ios-view-test` — all
  green, provable on Linux). The `fio_*` API already covers most of what the
  extension needs:
  `fio_new_game(seed, seed_len, n_players)` · `fio_apply_json(actor_seat,
  move_json)` (per-action actor seat — exactly what chain replay needs) ·
  `fio_legal_moves_json(seat,…)` · `fio_actor_mask()` · `fio_state_json` /
  `fio_public_state_json` · `fio_game_over()` · `fio_replay_encode_b32`
  (v5 share code) · `fio_last_reject()`. Read `cnitro/ios/include/ios_api.h`
  top-to-bottom — it is short and it is the contract.
- **Action wire**: `cnitro/src/awire.{h,c}` — the packed move format the
  whole product uses (browser→server bytes). `u8 kind, u8 n, n×card,
  [n×attack-target for cover]`. Self-delimiting, variable length,
  multi-card moves are ONE action. This replaces the older docs' "3 bytes
  per action" sketch — see §3.2.
- **Replay codec**: v5 frozen; **v6 shipped** (`replay_encode_v6`,
  `cnitro/src/replay.h:136`; wasm export `wasm_replay_encode_v6`,
  `cnitro/Makefile:432`; TS `encodeReplayV6` in
  `supabase/functions/_shared/replay/encode.ts`; `e2e/replay_v6.test.ts`).
  v6 supports a **mid-game cut** (explicit atom count, no
  `REPLAY_EINCOMPLETE`) — see §3.3 for what that means here.

### 2.2 The host iOS app — built (pending first Mac build)

`ios/` contains the full SwiftUI app per `IOS_APP_DESIGN.md`: `FoolishKit`
framework with `Engine/` (EngineC bridge, Models, LocalGame), `Boards/`
(TableView, FCard, FHandFan, FBattleGrid, FSeatBadge, FDeckWell, FActionBar),
`DesignSystem/` (Tokens, FStrings en/ru/ko, Haptics, FernCardBack, QRCode).
The extension will reuse ALL of it. `ios/project.yml` (XcodeGen) currently
defines targets `Foolish`, `FoolishKit`, `FoolishTests` — **no
`FoolishMessages` target yet** (a comment in project.yml already reserves
the `APPLICATION_EXTENSION_API_ONLY` step for this milestone).

### 2.3 Web/server plumbing the `/m/` route will reuse

- Kernel-in-browser: `supabase/functions/_shared/sdk/ts/wasm/engine.ts` (module
  `rules_wasm.ts`), wired to the client via `src/wasm/clientGuards.ts`
  patterns.
- base32: `base32Encode/base32Decode` in
  `supabase/functions/_shared/replay/codec.ts` (QR-alphanumeric-safe,
  URL-safe — reuse, do not invent).
- e2e harness: `e2e/*.test.ts` runs the real wasm kernel under Node
  (`npm run test:e2e`). Pattern-match `e2e/replay_v6.test.ts` and the
  hostile-bytes idioms in `e2e/client_guards.test.ts`.

### 2.4 What does NOT exist yet (your work, §4)

`cnitro/src/msg_wire.{h,c}` · `wasm_msg_encode/decode` exports · TS bridge
functions · `e2e/msg_wire.test.ts` / `e2e/msg_concurrency.test.ts` ·
`src/app/m/[payload]/page.tsx` · `FoolishMessages` target + extension UI ·
App Group storage · the Swift chain-cache/pending-ledger/rebase layer ·
`fio_msg_*` additions to the iOS shim.

---

## 3. Spec corrections (deltas against `IMESSAGE_GAME_DESIGN.md`)

The 2026-07-13 spec predates two things: the v6 codec landing and a close
reading of the deal-seed API. Apply these corrections; everything else in
that doc stands.

### 3.1 The deal seed is 32 bytes, not 16

`game_set_deal_seed_bytes` **requires len ≥ 32** (ChaCha-256 key; fewer
bytes silently falls back to the legacy 32-bit LCG — catastrophic here
because both devices MUST reproduce the identical wide deal).
`REPLAY_FORMAT6_HIDDEN_STATE.md` already flags the spec's "16 bytes" as
wrong. **FMSG v1 therefore carries a 32-byte seed**; the envelope offsets
from `IMESSAGE_GAME_DESIGN.md` §4.1 shift accordingly:

```
offset  size  field            (unchanged fields elide their notes — see §4.1)
0       1     magic      0xF7
1       1     format     1
2       1     flags
3       1     phase      0 WAITING · 1 ACCEPT · 2 LIVE · 3 FINISHED
4       8     game_id
12      2     turn       u16, count of kernel ACTIONS applied
14      1     last_actor_seat
15      1     n_players  2..8
16      1     variant    0
17      1     round      completed-round counter (rebase guard input)
18      8     parent8    first 8 bytes of SHA-256(previous envelope bytes)
26      32    seed       -> game_set_deal_seed_bytes(seed, 32)   [was 16]
58      1     n_joins
59      var   joins      n_joins × { u8 seat, u8 name_len<=12, name utf8 }
var     2     n_actions  u16
var     var   actions    seat-prefixed awire frames (§3.2)
```

### 3.2 Actions are seat-prefixed awire frames, not 3-byte triples

The old §4.2 sketched fixed `{type, card_a, card_b}` triples. Two problems:
multi-card moves (multi-attack, multi-pass) are single kernel actions with
n>1, and a chain interleaves DIFFERENT seats' actions (several seats act in
one round), so each action needs its actor. Store each action as:

```
u8 seat  |  awire frame (u8 kind, u8 n, n×u8 cards, [n×u8 attacks — cover only])
```

awire (`cnitro/src/awire.h`) is self-delimiting given `kind` and `n`, so no
per-action length prefix is needed. Decode = `awire_decode` (already
hardened against hostile bytes: clamped card ids, exact-length check);
apply = the same switch the shim uses. Size math: attack/cover of one card
= 4/5 bytes; typical mid-game payloads stay ~250–450 base32 chars, and the
§4.4 guardrail (P95 full game < 1,000 chars at 4 players) still holds —
keep the e2e assertion.

### 3.3 "Format 6" reality check — what the recent codec work gives you

The owner's summary is right in spirit: **the codec now supports mid-game
encodings**. Precisely:

- **Replay v6 shipped** (`REPLAY_FORMAT6_HIDDEN_STATE.md`) as the
  hidden-state-lossless replay: every hidden card's identity is
  entropy-coded inline, an explicit atom count lets a stream **terminate
  mid-game** (fool byte = 0xFF), and the whole menu/weight/atom machinery is
  production-proven at ~787k assertions. Server game-end now emits v6.
- **But v6 is NOT the iMessage continuation format.** It deliberately
  carries **no deal seed** (option 3 in that doc): a mid-game v6 stream
  reveals cards dealt SO FAR but says nothing about the undealt stock, so
  two devices cannot continue drawing identically from it. Serverless play
  needs the future too → **FMSG v1 = seed + action chain stands** (decode =
  re-deal from seed + replay actions; §3 of the game design doc).
- What v6 DOES give this project, concretely: (a) the **FINISHED bubble**
  can carry a v6 code so the fool's never-played hand and all draw timing
  are exact for the Oracle (the iOS shim's `fio_replay_encode_b32` currently
  emits v5, which decoders still accept — adding a
  `fio_replay_encode_v6_b32` is a small optional task, M4); (b) the
  **proven mid-game-cut machinery** (explicit count, version-dispatched
  decode) is the template for **FMSG v2** — the entropy-coded body
  (seed in header + rANS-coded actions against legal-move menus,
  ~1–2 bits/action) that §16 of the game design doc defers until the size
  guardrail trips. Do not build FMSG v2 now; do keep FMSG v1's version byte
  discipline so it can slot in.

### 3.4 Superseded host-app note

§9.1's "minimal host app" is long superseded: the host is the FULL app in
`ios/` (see its §17 status). "Host app" in the old doc = today's `Foolish`
target. The one-app-record decision (bundled extension, never standalone)
stands and is already reflected in the bundle ids.

### 3.5 Verified platform facts (July 2026 — cite these, not memory)

- **`MSMessage.url` cap is documented: 5,000 characters** (Apple docs,
  MSMessage.url). Our P95 guardrail (<1,000 base32 chars) sits at ~⅕ of the
  platform cap. Custom schemes forbidden; https required — ours is a real
  web page by design.
- **MSSession semantics confirmed by the docs:** a same-session send removes
  the previous bubble and inserts the new one at the transcript bottom; a
  summary line is left in the old position **only if `summaryText` was
  non-nil** — so ALWAYS set summaryText, or the game's history evaporates
  from the thread.
- **Compact style = the keyboard area:** no text fields there (no keyboard
  available — the nickname entry in the join flow must
  `requestPresentationStyle(.expanded)` first) and no horizontal scrollers.
- **No background execution, no push; `didReceive` fires only while
  visible** — as the spec assumed.
- **Messages extensions remain supported and reviewable in the iOS 26 era**
  (no deprecations in the Messages framework). Discoverability is poor
  (buried under the "+" menu since iOS 17): the HOST APP drives installs —
  do not build a growth plan on the Messages drawer.
- **Bubble image:** 300×195 pt @3x render guidance stands (WWDC16); Messages
  crops ~6 pt off the sides and recompresses — never bake text into the
  image; captions carry the words.
- **Shipping the extension later is a normal update** to the app record —
  M5 here does not need to ride the app's first submission.
- Two kernel-side cautions: the durable **state blob does NOT carry the deal
  seed** (only a deterministic-deck flag + remaining deck order) — cache the
  envelope, never "the blob + assume seed"; and **guards.wasm cannot back
  the `/m/` route** (built without `legal.c`/`replay.c` and with
  `-DDEAL_RNG_DISABLED`, so it cannot re-deal from a seed) — the route
  lazy-loads the full rules module via the engine bridge, exactly as §4 M0.6
  specifies.

---

## 4. Build order

Dependency spine: **M0 → M1 → M2 → M3**; M4/M5 are short tails. M0 needs no
Mac and de-risks the entire design in CI — start there.

### M0 — the wire + the model, all in this repo's existing toolchains (5–8 d)

1. **`cnitro/src/msg_wire.{h,c}`** — encode/decode of the §3.1 envelope.
   Style-match `awire.c` (bounds-checked, no allocation, hostile-input
   safe). Decode NEVER constructs a `Game` by memcpy: it returns the parsed
   fields + action list; validation happens by replaying through public
   kernel calls (`game_set_deal_seed_bytes` → deal/start → per-action
   apply), exactly like the shim does. Include SHA-256 (a small local
   implementation or reuse if one exists in-tree — grep first) for
   `parent8` and the Rule P digest tiebreak.
2. **Native test** `cnitro/tests/msg_wire_test.c` (add to `make difftests`):
   round-trip encode→decode→re-encode byte-identical at 2/3/4/8 players and
   every phase; tamper matrix (flip each byte class → clean failure);
   replay-validation rejects illegal chains.
3. **Wasm exports** `wasm_msg_encode` / `wasm_msg_decode` added to the
   rules build's export list (`cnitro/Makefile` — pattern-match
   `wasm_replay_encode_v6` at `:432`); rebuild the committed modules the
   same way existing codec changes did (see the v6 commit for the recipe).
4. **TS bridge** in `supabase/functions/_shared/sdk/ts/wasm/engine.ts`
   (`kernelMsgEncode/kernelMsgDecode`, pattern: `kernelReplayEncodeV6`).
5. **e2e**: `e2e/msg_wire.test.ts` (round-trips, goldens as hex fixtures,
   tamper matrix, the < 1,000-char P95 size guardrail at 4 players) and
   `e2e/msg_concurrency.test.ts` — implement Rule P + Rule R (rebase with
   the round-boundary guard) as PURE TS functions over the wasm exports,
   then property-test: total order; delivery-order independence
   (all interleavings converge); the seven worked examples of
   `IMESSAGE_GAME_DESIGN.md` §14 as named regression cases (pickup ∥
   throw-in asserted in BOTH delivery orders); 3–8 player fuzz — every
   convergent state legal, every discarded action mapped to a §7.4 reason.
   **These pure functions are the reference implementation the Swift port
   must match fixture-for-fixture in M3.**
6. **`/m/` route** `src/app/m/[payload]/page.tsx`: client-side only, parse
   the leading text-version char, `base32Decode`, `kernelMsgDecode`,
   render the PUBLIC view read-only with existing board components + the
   install/play CTAs (mockup M7). OG meta tags with the public snapshot.

### M1 — Xcode target + shim additions (2–4 d, first Mac work)

1. `cnitro/ios/ios_api.c`: add `fio_msg_decode_json` (payload bytes → JSON:
   envelope fields + per-seat view after replay + my legal moves),
   `fio_msg_encode` (current game + envelope fields → payload bytes), and
   `fio_replay_encode_v6_b32` (optional, M4 can absorb). Extend
   `ios_api_smoke.c`; keep `make ios-smoke` green on Linux.
2. `ios/project.yml`: add the extension target —
   ```yaml
   FoolishMessages:
     type: app-extension.messages
     platform: iOS
     sources: [FoolishMessages]
     dependencies: [{ target: FoolishKit }]
     settings:
       base:
         PRODUCT_BUNDLE_IDENTIFIER: cards.foolish.app.MessagesExtension
         INFOPLIST_FILE: FoolishMessages/Info.plist
   ```
   plus `Foolish` gains `dependencies: [{ target: FoolishMessages, embed: true }]`,
   both targets gain the App Group `group.cards.foolish` entitlement, and
   FoolishKit turns on `APPLICATION_EXTENSION_API_ONLY` (the project.yml
   comment marks the spot; fix any app-only API fallout by moving those
   calls into the app target).
3. XCTest: golden parity — the M0 hex fixtures decode identically through
   `libfoolish.a`.

### M2 — the extension UI (5–8 d)

Build the screens of `docs/imessage-layout.html` with FoolishKit components:
compact drawer (M3: App-Group game list + New game), expanded table (M4:
seat chips / battle grid / deck well / fan — TableView's grammar, ≤4 seats),
the bubble snapshot renderer (300×195 pt `UIGraphicsImageRenderer`, PUBLIC
state only, per the mockup page §1 snapshot zone spec: watch-style glyph-card rows +
wool/fern materials), lobby (M6). Interaction: tap-to-attack,
arm-then-target to cover, kernel-driven action bar, Send-move enabling on
≥1 applied action with the "others may be playing too" hint.

### M3 — Messages wiring + the concurrency layer (7–10 d)

`MSMessagesAppViewController` lifecycle per `IMESSAGE_GAME_DESIGN.md` §11
(the callback table is exact). Implement in Swift: chain cache + pending
ledger in the App Group (§9.3 JSON shape), Rule P adoption, Rule R rebase
with the round-boundary guard, staged/`didStartSending`/`didCancelSending`
handling (mockup M4b), seat identity layers §6 (cache → sender inference →
nickname picker). **Port the M0 pure functions with the same JSON fixtures
run in both CIs** — the e2e suite is the oracle. Manual matrix: two
simulators (Xcode Messages harness), then a device pair (§17.12 warns the
timing differs).

### M4 — game end + localization (2–3 d)

FINISHED bubble (mockup M5): terminal-action device encodes the replay
(v6 preferred — add the shim entry if M1 skipped it), `message.url` = the
standard `foolish.cards/<code>` (NOT `/m/`), trophy row in the drawer.
All strings through the shared localization keys (FStrings/xcstrings;
en/ru/ko in the same commit). `summaryText` uses conversation-neutral
phrasing (§17.13).

### M5 — review prep (3–5 d)

Extension icon set (fern mark), privacy labels (extension adds nothing —
still "no data collected" for iMessage play), age rating unchanged (card
game, no gambling; bot-name rationale in `IOS_BOT_NAMING.md` §5), TestFlight
device-pair pass, submit riding the app's record.

---

## 5. Invariants checklist (verify before every merge)

- [ ] No Durak rule in Swift or TS — every legality/turn answer via kernel.
- [ ] The payload is always the FULL chain from the deal (chains, not diffs).
- [ ] Delivery order is never trusted — Rule P decides, everywhere, always.
- [ ] No action survives rebase across a round boundary (Rule R guard).
- [ ] The bubble snapshot renders PUBLIC state only (it appears in
      notifications and on lock screens).
- [ ] `insert` stages; only the human sends; both `didStartSending` and
      `didCancelSending` paths tested.
- [ ] Participant UUIDs never enter the payload (device+conversation scoped).
- [ ] Envelope decode is hostile-input safe (tamper matrix green) and
      validation = full kernel replay — no partial recovery.
- [ ] Seed is 32 bytes; a shorter seed must be rejected at encode AND decode.
- [ ] v5 replay wire untouched; FMSG carries its own version byte.
- [ ] Extension memory stays SwiftUI + libfoolish only — no wasm, no
      WKWebView, no bots, no Oracle in-extension (§17.5).

## 6. Where everything lives (quick index)

| Thing | Path |
| --- | --- |
| Full protocol/concurrency spec | `docs/IMESSAGE_GAME_DESIGN.md` |
| This work order | `docs/IMESSAGE_IMPLEMENTATION_HANDOFF.md` |
| UI mockups + snapshot spec | `docs/imessage-layout.html` |
| Phone-app layout study (board grammar) | `docs/ios-phone-layout.html` (+ `docs/IOS_PHONE_LAYOUT.md`) |
| Kernel | `cnitro/src/{game,legal,view,replay}.c` |
| Seeded deal API | `cnitro/src/game.h` (§ RNG, `game_set_deal_seed_bytes`) |
| Action wire | `cnitro/src/awire.{h,c}` |
| iOS shim + build | `cnitro/ios/`, `cnitro/Makefile` (`ios-lib`, `ios-smoke`) |
| v6 codec + doc | `cnitro/src/replay.{h,c}`, `docs/REPLAY_FORMAT6_HIDDEN_STATE.md` |
| wasm bridge (TS) | `supabase/functions/_shared/sdk/ts/wasm/engine.ts` |
| base32 | `supabase/functions/_shared/replay/codec.ts` |
| e2e harness | `e2e/` (`npm run test:e2e`) |
| Host app + design system | `ios/` (`project.yml`, `FoolishKit/*`) |
| App design + status | `docs/IOS_APP_DESIGN.md` (§16 how-to, §17 status) |
