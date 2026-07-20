# iMessage deferred designs — batch 7

*Design sketches for `docs/HARNESS_NOTES_TRIAGE.md` notes 15, 16, 22–24, 26, 27
— the seven items marked `[D]`/`[A]` (deferred/answered, no code). This batch
is docs only; nothing below has been implemented. Every claim about existing
code carries a `file:line` anchor verified against the tree on 2026-07-20
(branch `claude/test-harness-notes-triage-js4y8q`, after batch 6 `3e296f2`).
Where a cited design doc has since been superseded by a later one, the later
doc is what's quoted — `IMESSAGE_GAME_DESIGN.md` §4's byte offsets in
particular are stale; the current wire is `c/src/msg_wire.h`'s own header
comment plus `docs/IMESSAGE_BODY_CODEC.md`.*

---

## 1. Fair-tempo enforcement (owner's notes 22–24)

Three honor-system timing rules for iMessage-only play (no server, so nothing
here can be trust-hardened — see §17.8/§17.9 of `IMESSAGE_GAME_DESIGN.md`,
already accepted for hidden-info peeking and payload spoofing):

- **(a)** No pickup within 10s of the bout's first attack.
- **(b)** 120s since the last cover landed with no resolution → the next
  "good" advances the round anyway (attackers had their chance): discard,
  draw, shift defender.
- **(c)** Defender AWOL ~5 minutes → the next "good" forces their pickup and
  advances the round (connection-loss recovery, deliberately long).

**The three rules are not the same shape**, and that difference decides the
whole implementation path. Working through it:

### 1.1 Where a timestamp would live — and the case for not adding one yet

The current wire (`c/src/msg_wire.h:13–31`, the authoritative layout — the
byte offsets in `IMESSAGE_GAME_DESIGN.md` §4.1 are the *superseded* 16-byte-seed,
raw-action draft; the seed is 32 bytes and the body is a v6 replay code, per
the ⚠ banners in that doc and `docs/IMESSAGE_BODY_CODEC.md`):

```
off  size  field
0    1     magic 0xF7        16   1  variant   (reserved, =0 today)
1    1     format 2 (v6)     17   1  round
2    1     flags             18   8  parent8
3    1     phase             26   32 seed
4    8     game_id           58   1  n_joins
12   2     turn               59  var joins
14   1     last_actor_seat    var 2   n_actions
15   1     n_players          var var body (v6 replay code)
```

`MSG_HEADER_LEN` is 59 bytes through `n_joins` (`msg_wire.h:115`); nothing in
that fixed header is a wall-clock value. Measured envelope sizes
(`docs/IMESSAGE_BODY_CODEC.md` §2, shipped/confirmed): median v6-body
envelopes run **~119 B at 2p, ~130 B at 4p, ~153 B at 8p** (≈192/208/248 base32
chars), and `docs/IMESSAGE_SHIP_BLOCKERS.md`'s protocol-level 2p full-game
proof measured bubbles **≤108 B** end to end (`e2e/msg_full_game.test.ts`,
59 turns). Two `uint32` timestamps (bout-start epoch, last-cover epoch — 8
bytes) would be a **+7–9% raw-byte tax on a typical envelope**, before base32
inflates it further — not free, and squarely the kind of format change
`docs/IMESSAGE_BODY_CODEC.md` exists to warn against doing casually (see its
finding 1: the *raw* body alone missed budget by 1.3x and had to be replaced
wholesale).

More importantly, **the kernel already has a real precedent for keeping
wall-clock values out of the wire, and it argues against adding one here.**
`Game` carries `has_good_timestamp` (`c/src/game.h:167`) — a **bool**, not a
timestamp — and the reason is spelled out at the one place it's serialized:
`c/src/json_out.c:82`, *"Whether a good_timestamp EXISTS, not its value: the
value is a host clock…"*. The TS host stores the actual `Date.now()` value
itself (`src/state/clientReconcile.ts:35`, consumed only for an animation
delay at `src/contexts/AnimationContext.tsx:797`); only the boolean crosses
into the deterministic `Game` struct and onto the wire (`c/src/view.c:31,92`).
The kernel's own design already draws this line: **time is a host/UI concern,
not a kernel/wire one**, unless a rule genuinely needs to *replay* to the same
answer on every device — which brings us to the actual fork:

### 1.2 Sender-local clocks are the only clocks that exist

There is no server (`IMESSAGE_GAME_DESIGN.md` §1: "an iMessage extension has
no server in the loop"; §17.7: "No push, no background: if B never taps the
bubble, the game just waits"). Every clock reading is one device's own clock,
read at the moment *that device* observes an event. Two consequences:

- **Rules (a) and (b) never need to compare one device's clock to another's.**
  Each device decides, using only its own clock, "has *my* local reading of
  wall-time moved past threshold X since *I* locally observed event Y?" —
  a single-clock comparison, immune to skew between players' phones. This is
  exactly why they're implementable as **pure client-side gating/orchestration
  with zero wire representation** (below).
- **Rule (c)'s "AWOL" claim is inherently a claim about someone else's
  silence**, which only becomes representable in the wire once one device
  needs to *tell* other devices "I waited N minutes and nothing came" — and
  that claim is unverifiable by construction (no NTP, no trusted third party).
  This is fine for a casual, honor-system game (§17.8/§17.9 already accept
  worse: real hidden-hand peeking), but it means any v2 wire encoding for (c)
  should carry a **self-reported elapsed duration**, not an absolute epoch —
  avoids ever comparing two different devices' clocks, and keeps the "claim"
  shape consistent with every other honor-system trust boundary this protocol
  already has.

### 1.3 Which side enforces — and why (a)/(b) need no kernel change at all

`IMESSAGE_GAME_DESIGN.md` §10 already establishes the UI pattern: "action bar
shows the kernel-driven set of non-card moves… enabled strictly from the
legal-move list." All three rules can be framed as an *additional, local-only*
disablement layered on top of that kernel-driven set — but they are not
equally cheap to actually enforce as *state changes*, because (b)/(c) don't
just hide a button, they change what a legal action *does*.

**(a) is trivially UI-only.** `handle_pickup` (`c/src/game.c:901`) has no
timing check today and needs none added: the defender's device simply keeps
"Pick up" disabled in its own action bar for 10s after it locally observed the
bout's first `LOG_ATTACK`. Nothing crosses the wire; nothing for Rule P/R
(`c/src/msg_wire.h:238–292`) to reconcile. A hand-edited client could always
send pickup at t=2s — accepted, same trust level as everything in §17.9.

**(b) is UI *orchestration* of an already-legal action sequence — also zero
wire change.** `handle_good` (`c/src/game.c:976`) only requires that the
calling seat isn't the defender, isn't the empty-table first-attacker, and
hasn't already said good (`ENGINE_REJECT_NOT_DEFENDER`/`FIRST_MUST_ATTACK`/
`ALREADY_GOOD` — none of them check *which device* is submitting the action).
Actions in an FMSG chain are seat-prefixed, not device-prefixed
(`msg_wire.h`'s header comment; `IMESSAGE_GAME_DESIGN.md`'s handoff-corrected
§4.2), and §17.9 already documents that a device can include an action
attributed to a seat it doesn't own — that's the accepted spoofing surface.
So after a 120s stall, the impatient attacker's device can locally call
`handle_good` **once per still-pending attacker seat, then its own**, entirely
through the kernel's public API, and seal the result. Every other device
replays that as an utterly ordinary `good, good, …, good → all_good &&
all_covered → execute_round_transition` chain (`c/src/game.c:949,1005`) — **it
is not a special case**, which is exactly the constraint this design has to
satisfy. Rule P (`msg_wire.h:238–264`) and Rule R (`:266–292`) need nothing new:
if the real attacker's own "good" is in flight concurrently, ordinary Rule P
(higher `round` wins — a round-closing chain outranks a non-closing one) picks
between them the same way it already picks between a pickup and a throw-in
(`IMESSAGE_GAME_DESIGN.md` §7.5).

**(c) is the one that genuinely cannot be done as an "ordinary legal chain"
today, and the note's framing undersells that.** `handle_pickup`
(`c/src/game.c:901`) rejects immediately if `player_idx != g->defender`
(`ENGINE_REJECT_NOT_DEFENDER`) — no other seat may submit a legal pickup for
the defender, full stop, and (unlike "good") there's no way to route around it
through repeated legal single-seat actions, because a partially-uncovered
table (the AWOL scenario — the defender hasn't finished covering) can never
reach `all_covered`, so no sequence of `good`s can trigger
`execute_round_transition` either. **A literal "force their pickup" needs a
new kernel primitive** — e.g. relaxing `handle_pickup` to accept a
non-defender seat's call, gated on a wire-carried, self-reported elapsed
duration since the last cover — which is a genuine rules change with the same
shape (and the same replay-codec ripple effects) as note 16's auto-discard in
§3 below, including needing the reserved `variant` byte
(`msg_wire.h:172`/`MSG_EVARIANT`) to gate it so old chains are unaffected. It
is not v1 scope.

### 1.4 Recommended phased path

- **v1 (this batch validates, does not build): UI-gating, no wire change.**
  - (a): disable "Pick up" locally for 10s after observing the bout's first
    attack.
  - (b): after 120s of no resolution, surface a "the round can move on"
    affordance that, when tapped, submits the synthesized multi-seat `good`
    chain described in §1.3 — an honor-system nudge a human explicitly
    triggers, not a silent timer-driven auto-action (keeps intent visible in
    the transcript, per `summaryText` conventions, §17.4).
  - (c): **descope the "force" to a nudge.** Show the other seats a banner —
    "Sveta hasn't moved in 5+ minutes" — with a "start a new game instead"
    escape hatch. Do not attempt to synthesize her pickup; the kernel doesn't
    allow it without a new primitive (§1.3).
- **v2: encoded timestamps**, only if (c) becomes worth building for real —
  a new action kind gated by the `variant` byte, elapsed-duration-based (not
  absolute epoch, §1.2), logged/shaped identically to an ordinary
  `LOG_PICKUP` so nothing downstream (replay v6, Rule P/R, bots) has to learn
  a new state shape — only the *legality check* differs. Cross-reference §3:
  this and the auto-discard variant would share one mechanism (the reserved
  `variant` byte), so if both ever ship they should probably ship as bits of
  one small rules-variant bitmask, not two separate format bumps.

---

## 2. Octogen research idea (owner's note 15)

Note 15 asks: *"what if rollout was random? or cheater?"* — this is a question
about octogen's **Monte-Carlo rollout policy** (the fixed strategy used to
play out *sampled* worlds during search), not about octogen's identity as a
bot. The codebase already has almost everything needed to run this.

### 2.1 What already exists

`octogen_strategy.c`'s `og_rollout_for` (`:765–776`) is a **research knob that
already poses this exact hypothesis** in its own comment:

> *"Research knob for the 'rollout-policy bias' hypothesis — vs a strong
> opponent, a weak (handwritten) rollout policy biases value estimates, so
> more worlds saturates. A stronger rollout policy may reduce that bias."*

`OG_ROLLOUT` (parsed at `:1559`) currently supports:

- `0` (default): stage-aware — handwritten while the deck is alive or the
  game is heads-up, espresso once it's a multi-player deck-empty endgame
  (`:773–775`).
- `1`: espresso everywhere.
- `2`: handwritten everywhere.

There is **no `3` for a uniform-random rollout today** — that's the one line
missing (`random_strategy_choose`, `c/src/random_strategy.c:5`, already a
registered `STRAT_RANDOM` bot, just never wired into `og_rollout_for`).

**"Cheater" is two different, easily-conflated things in this tree — the
design must not merge them:**

1. **Espresso-as-rollout (already `OG_ROLLOUT=1`)** cheats only *inside the
   sampled world*: per `firecracker_strategy.h:6–12`, it plays optimally
   against hands *robusta/octogen itself made up* for that Monte-Carlo trial
   — "what's my best move if my opponents had oracle vision of the fictional
   hands I'm sampling?" It is not an upper bound on octogen vs the real game;
   it's a *stronger-rollout* arm of the same bias question the code comment
   already asks.
2. **`octogen_oracle_strategy_choose`** (`octogen_strategy.c:1877`, registered
   `strategy.h:74`, CLI alias `octogen_oracle`/`ogo` per
   `strategy.h:103` and `nxn_render.py`'s `CODE` map) is the **true upper-bound
   oracle** — it sees the *real* hidden hands, not sampled ones. This is what
   note 15's "cheater" almost certainly means, and it's not a rollout-policy
   variant at all: it's a full top-level strategy already in the roster.

### 2.2 The experiment, concretely

Three arms, same opponent roster, same seeds (paired comparison — see §2.3):

| Arm | What it measures |
| --- | --- |
| **(A) octogen, `OG_ROLLOUT=0`** (shipped default) | Baseline: today's stage-aware rollout policy. |
| **(B) octogen, `OG_ROLLOUT=3`** (new: uniform-random rollout — the one line to add) | Floor: how much win rate is lost if the rollout policy carries *no* skill at all. Tests the bias hypothesis the code already poses, at its most extreme point. |
| **(C) `octogen_oracle`** (already shipped, real hidden-hand access) | Ceiling: the total value octogen is leaving on the table from imperfect information *of any kind* — not rollout-policy-specific, but the natural upper bound to size the gap against. |

Reading the three together: **(C) − (A)** is the theoretical ceiling any
rollout-policy improvement could ever close; **(A) − (B)** is how much of
today's octogen strength the *current* rollout policy already buys (per the
existing comment's hypothesis, expect this to be a real, positive, probably
sizeable gap — a uniform-random rollout should meaningfully bias value
estimates toward noise). If (A) turns out close to (B), the hypothesis in the
existing comment is wrong and rollout-policy quality barely matters relative
to raw simulation count — a genuinely useful negative result either way.

### 2.3 The harness already in the tree

No new tooling is needed, only new invocations:

- **`c/tools/nxn_matrix.sh`** — the round-robin win-rate matrix
  (`STRATS=(... octogen octogen_oracle ...)`, line ~17) already includes arm
  (C) against arm (A) and every other bot for free; it drives
  `build/cnitro_eval --strategy=A --opp=B --players=2 --games=N
  --seed-start=S` (`c/src/main_eval.c:1–13` documents the CLI) per cell,
  parallelized, resumable.
- **Arm (B)** needs the one-line addition to `og_rollout_for`
  (`octogen_strategy.c:765`, `if (og_rollout_policy == 3) return
  random_strategy_choose;`) plus running `cnitro_eval --strategy=octogen
  --opp=<roster>` with `OG_ROLLOUT=3` in the environment — the harness doesn't
  care that the "strategy" is still named `octogen`; the env var is what
  changes its internals, same pattern already used for `OG_ROLLOUT=1/2` today.
- **Paired-seed methodology**: `c/tools/hide_tax/hide_eval.c` +
  `analyze.py` is the exact template for this kind of "does a policy tweak
  help, controlling for the deal" question — it self-plays the same seeds
  under two configurations and reports a paired delta with a z-score
  (`docs/OCTOGEN_HIDE_UNCOVERABLE.md`'s own results: 5,999 paired games,
  win-rate delta +1.38%, z=+3.36). Reusing that harness (or its shape) for the
  rollout-policy question gives tight, variance-reduced confidence intervals
  without needing an enormous game count — that doc's own numbers are the
  calibration for how many paired games this experiment would plausibly need
  for a similarly modest effect size.
- `docs/OCTOGEN_PC2_DIVERGENCE.md` was read for context but doesn't bear on
  this question directly — it measures transposition-table size vs a
  reference, not rollout-policy choice; noted so as not to imply it does.

---

## 3. Same-value auto-discard speedup (owner's note 16)

Rule: if every attack on a full table shares one value **and** every cover
shares one value, skip the "everyone says good" round trip and discard
immediately.

### 3.1 Where it would live

The natural insertion point is `handle_cover` (`c/src/game.c:725`), at the
`all_covered` branch that already exists (`:823–833`):

```c
bool all_covered = (g->num_battles > 0);
for (int i = 0; i < g->num_battles; i++) {
    if (!!card_is_none(g->table_battles[i].defense)) { all_covered = false; break; }
}
if (all_covered) {
    g->has_good_timestamp = true;
    for (int i = 0; i < g->num_players; i++) {
        if (i != g->defender && g->players[i].status == PLAYER_STATUS_IN)
            g->players[i].awaiting_attack = true;
    }
}
```

Today this just arms every attacker's `awaiting_attack` flag and waits for
each of them to call `handle_good` (`:976`), which only transitions the round
once `all_good && all_covered` (`:1005`, via `execute_round_transition`,
`:949`). The speedup would add a same-value check right here — reusing the
existing `all_same_value` helper (`:648`, currently used only to validate that
a *first* attack's own cards share one value) generalized to sweep
`table_battles[i].attack.value` across all `i`, and separately
`table_battles[i].defense.value` — and if both hold, **synthesize a
`LOG_GOOD` for every in-status non-defender attacker and call
`execute_round_transition(g)` immediately**, rather than waiting for real
`good` actions. Synthesizing the goods (instead of skipping straight to
`execute_round_transition` with no goods at all) matters — see §3.2.

**`legal.c` needs no change.** `calculate_legal_moves`
(`c/src/legal.c:358–397`) always computes off the *current* `g->num_battles`/
`good_players_mask`; since the auto-discard fires and clears the table
*inside* `handle_cover`, before control ever returns to a legal-move query,
every device's next `calculate_legal_moves` call simply sees the already-reset
post-round state — the `MOVE_GOOD` the bot menu currently always offers once
`is_def` is false and `num_battles > 0` (`:383–394`) is naturally absent
because `num_battles` is already back to 0. This keeps the blast radius to
`game.c`.

### 3.2 Why this is a RULES change, not a UI change

This is the crucial distinction from §1's rules (a)/(b): those were client-side
gating of when a legal action is *offered*; this changes what a legal action
*produces* — specifically, it removes required round-trip `good` actions from
the log entirely for qualifying rounds, and that ripples into every layer that
assumes today's shape:

1. **Replay determinism.** `IMESSAGE_GAME_DESIGN.md` §3.2's core equation —
   *"partial game ≡ (deal seed, ordered list of actions applied so far)"* —
   means the SAME action list must produce the SAME state on every device,
   forever. If two kernel builds disagree on whether same-value covers
   auto-discard, they derive different `round`/`turn` counts from an
   identical byte stream, which is exactly what `msg_replay`
   (`msg_wire.h:222–236`) is built to catch as a header/body mismatch
   (`MSG_EROUND`, `MSG_ETURN`) — an *unversioned* rules change would make that
   fire spuriously on old chains replayed by a new kernel, or silently diverge
   if it doesn't fire.
2. **The v6 replay codec's atom shape.** `replay.c:1011–1026` documents the
   *only* rule the encoder knows for folding goods into one `round_end` atom:
   *"a round_end marker for every DISCARD directly preceded by a GOOD"* — and
   the comment explicitly flags this rule as having had **four independent
   copies** (`encode.ts collectV6`, `ios_api.c build_encode_input`,
   `tests/replay_difftest.c`, `tests/replay_v6_test.c`) before being
   consolidated into the kernel. An auto-discard that does *not* synthesize
   the `LOG_GOOD` entries first would produce a **`DISCARD` directly preceded
   by a `COVER`** — a log shape the codec has never seen and has no atom for.
   Synthesizing the goods (§3.1) is what keeps this a no-op for the replay
   codec: the log stream still ends in `GOOD*, DISCARD`, so `log_atom_kind`
   still recognizes it. This must still be verified against golden vectors,
   not assumed — `docs/IMESSAGE_BODY_CODEC.md` finding 3 is a fresh, direct
   example of exactly this kind of log-shape assumption quietly breaking
   (47% of 4p mid-game cuts lost their pending-good state before that finding
   was fixed).
3. **Existing recorded games.** `docs/IMESSAGE_BODY_CODEC.md` notes "v5 must
   stay byte-frozen: existing v5 integers in `game_snapshots.moves` must
   decode byte-identically." An *unconditional* rules change to `handle_cover`
   changes what NEW games' logs look like at the exact moment covers land,
   which is fine for new games but has no business affecting how OLD ones
   (already encoded, sitting in `game_snapshots`) are interpreted if they're
   ever re-simulated/re-derived by a kernel that now behaves differently by
   default.

### 3.3 The safe rollout: the `variant` byte already exists for this

`msg_wire.h`'s envelope carries a `variant` field at offset 16 (`:172`,
`uint8_t variant;` in `MsgEnvelope`), documented as "reserved rules-variant
byte, =0 today" and explicitly earmarked for exactly this kind of thing
(`IMESSAGE_GAME_DESIGN.md` §14: *"The variant byte stays reserved for actual
rules options (deck size, transfer/perevodnoy), not for concurrency
workarounds"*). Right now it is **hard-pinned**: `msg_wire.c:78` —
`if (e->variant != 0) return MSG_EVARIANT;` — and every e2e test seals every
envelope with `variant: 0` explicitly and literally today
(`e2e/msg_concurrency.test.ts:54,200,287`; `e2e/msg_full_game.test.ts:75,116`;
`e2e/msg_lobby_v2.test.ts:48,60,71,88,117`) — confirming the byte is live,
tested, and currently doing nothing but rejecting non-zero values
(`MSG_EVARIANT -7`, `msg_wire.h:136`).

Rollout plan:

1. Define a variant constant (e.g. `MSG_VARIANT_AUTO_DISCARD_SAME_VALUE = 1`)
   and thread it from the envelope into `Game` (a new field, or a flags param
   passed alongside the deal seed at `msg_replay` time) so `handle_cover`'s
   auto-discard branch is gated `if (g->rules_variant & …)` — **variant 0
   games are byte-identical to today's behavior, unconditionally.**
2. Loosen `msg_wire.c:78` from a blanket reject to an allow-list bound
   (`if (e->variant > MSG_VARIANT_MAX) return MSG_EVARIANT`), and have
   `msg_replay` pass the variant into the fresh `Game` it deals — so the same
   envelope always replays under the same rules on every device, which is the
   wire's whole model (`msg_wire.h`'s own framing: "everything a device needs
   is here").
3. Both engines need the flag identically — `libfoolish.a` (iOS) and
   `rules.wasm`/`bots.wasm` (web/e2e) — extending the existing cross-engine
   parity discipline (`IMESSAGE_GAME_DESIGN.md` §8.2 golden vectors,
   `e2e/msg_wire.test.ts` as "the cross-engine gate" per
   `docs/IMESSAGE_BODY_CODEC.md` §7) with a variant=1 fixture.
4. A game's variant is chosen once at creation (like `n_players`/seed) and
   never changes — old games stay on variant 0 forever by construction; only
   a future "house rules" toggle at New Game would ever produce variant 1.
   This is what makes the rollout *safe*: nothing about existing chains
   changes unless their own header says so.
5. Needs its own golden-vector + tamper suite mirroring `e2e/msg_wire.test.ts`,
   plus a replay round-trip fuzz extending the `probe_v6_midgame`-style
   methodology in `docs/IMESSAGE_BODY_CODEC.md` to variant=1 games
   specifically, since that's exactly where a new log shape would surface.

(§1.4 already flags that a v2 "forced pickup" primitive for fair-tempo rule
(c) would want the same `variant`-byte mechanism — if both ship, they're
better designed as two bits of one small rules-variant bitmask than two
separate efforts.)

---

## 4. Privacy policy (owner's note 26)

Direct answer to the question as asked: **yes, App Store Connect requires a
privacy policy URL in the app record's metadata — for every app, regardless of
what it collects.** This is a store-submission requirement (the app record's
"App Privacy" / metadata fields), not a Messages-extension-UI requirement.
There is **no** requirement for an in-app or in-extension privacy link for an
app that collects nothing — that's a separate, optional convenience.

**Status update (2026-07-21): the URL half of this note is now DONE.** The
App-Store-submission branch (merged into this one) added the actual pages this
section says the store record needs — `src/app/privacy/page.tsx` +
`src/components/Privacy.tsx` → `foolish.cards/privacy`, and
`src/app/support/page.tsx` + `src/components/Support.tsx` →
`foolish.cards/support` — and `docs/IMESSAGE_APP_STORE_SUBMISSION.md` is the
full paperwork package that consumes them (its metadata table names both URLs).
Both pages must actually be DEPLOYED before those URLs go into App Store
Connect. What remains open is the *product* half below, unchanged.

What's already tracked in this repo, so batch 7 doesn't duplicate it:

- **`docs/IMESSAGE_SHIP_BLOCKERS.md`** already carries this as Chain-A item
  **A5** ("Compliance close-out") and Chain-B item **B5.3**: *"`PrivacyInfo.xcprivacy`
  added"* — three manifests exist today
  (`ios/FoolishKit/PrivacyInfo.xcprivacy` — load-bearing, declares the
  required-reason UserDefaults APIs `CA92.1`/`1C8F.1` because
  `MessageGameStore`/`FStrings` live in FoolishKit; lean top-level manifests
  in `ios/FoolishApp/` and `ios/FoolishMessages/`, tracking false, no
  first-party collection declared yet). What's still open per that doc is the
  *product* half — the `NSPrivacyCollectedDataTypes` entries and the matching
  App Store Connect privacy labels — which depends on the undecided v1 online
  scope (A2, the staging Supabase question). That doc is Mac-unverified on
  this point (no Xcode on Linux).
- **What this game actually collects, in v1**: nothing server-side. A game's
  entire state lives inside Apple's own Messages payloads
  (`IMESSAGE_GAME_DESIGN.md` §1: "the URL payload must carry the entire
  game" — no state on any server this project runs). The `/m/` web page
  (`src/app/m/[payload]/page.tsx`) is a static, client-side render of a
  payload the URL already contains — it makes no network call carrying game
  data, "client-side only… no auth, no database row" per that file's own
  header comment. The only place any account/PII exists at all is the
  separate, optional online layer of the main iOS app (Supabase auth +
  online play), which is exactly what A2/A5 are about — the iMessage
  extension itself is offline-only and collects nothing.
- **Delete-account machinery already exists** for that online layer:
  `server/impls/supabase/functions/delete-account/index.ts` (edge function) +
  migration `20260714120000_account_deletion.sql` (PII-scrub RPC), wired to
  `ios/FoolishApp` via `AccountService.swift:25–29`
  (`docs/IMESSAGE_SHIP_BLOCKERS.md` §1, §3 A5). One known open item worth
  carrying forward, not papering over: that same doc flags that
  `game_snapshots.extras`' replay-name blob is **not yet anonymized** on
  deletion — a decision (accept for submission, or schedule a replay
  re-encoding first) that A5 still needs to make.
- **Practical recommendation**: a Settings row linking to a hosted privacy
  policy in the main host app is cheap, conventional, and reduces reviewer
  friction even though it isn't strictly required for the extension — worth
  doing as part of A5's existing close-out work, not as new scope.

---

## 5. Link to the real app once it's ready (owner's note 27)

**Current funnel state, after batch 6.** A game bubble's URL is always a
`/m/1<base32>` payload link (never a bare `foolish.cards/<code>`, since batch
6 fixed the FINISHED-bubble-unparseable bug — `IMESSAGE_LOBBY_V2.md`,
"FINISHED bubble: `/m/` + the web funnel"). `src/app/m/[payload]/page.tsx`
decodes any phase read-only and renders a `Funnel` component
(`page.tsx:170–195`) with: a prominent **"🎬 Watch the replay"** CTA when the
game is FINISHED and a v6 replay code could be derived
(`kernelResidentReplayCodeV6`, falling back to a plain "This game has ended"
banner if derivation fails, so the funnel degrades gracefully rather than
breaking); always, a **"📲 Get Foolish on the App Store"** CTA; and a
**"…or play Durak free right here"** link back to the main web client. The
App Store CTA already points at a placeholder URL in code today
(`href="https://apps.apple.com/app/foolish"`, `page.tsx:192`) — it's wired,
just not live.

**What changes once the App Store listing exists.** First, the `/m/` page's
install CTA becomes a real, working link the instant the placeholder URL is
swapped for the real App Store ID — a one-line change, no new engineering,
since the CTA slot and copy already exist. Second, the FINISHED **in-extension**
state could grow a "Get the app" row — and this IS a tweak to an existing
screen: `FGameOverList` (the ranked results screen notes 18/40/41 polished,
`ios/FoolishKit/Boards/MessageTableView.swift`) is rendered by the extension's
own interactive board when a game the viewer opens is over (`MessageTableView`
is what `MessagesRootView` shows in the extension; since batch 3 it appears
after the final animation settles). The FINISHED *bubble* in the transcript
is separate — a fool-announcement summary string
(`ios/FoolishMessages/MessagesViewController.swift`) whose tap opens that
board. So the row is one more `FButton` under the existing New-game button in
`FGameOverList`, plus the same App Store URL the `/m/` page uses. Third, Messages' own app drawer
listing (the "+" tray other iMessage apps show up in) is entirely handled by
Apple once the app has a live App Store record with the Messages extension
target attached (`IMESSAGE_GAME_DESIGN.md` §9.1's bundled-app decision) — no
engineering work in this repo makes that happen; it's a platform mechanic
that activates on approval.

---

## Cross-cutting note

Sections 1 and 3 both converge on the same mechanism — the `variant` byte
reserved in `msg_wire.h` and currently hard-pinned to 0 at `msg_wire.c:78` —
as the safe way to ship a real rules change without touching old chains. Any
future work on either should design them as bits of one small rules-variant
scheme rather than as two independent format bumps.
