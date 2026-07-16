# The FMSG body must be a v6 code — two findings that force it

*Written 2026-07-16 from the iMessage M0 branch (`claude/imessage-m0`), which
implements `cnitro/src/msg_wire.{h,c}` per
`IMESSAGE_IMPLEMENTATION_HANDOFF.md` §4 M0. Every number below is MEASURED by
`cnitro/tests/msg_wire_test.c` (`make build/msg_wire_test && ./build/msg_wire_test 60`)
over 240 completed games per configuration, played by `robusta` (the bot humans
actually face), not estimated.*

> **M0 IS DONE, 2026-07-16.** Findings 1-3 are closed and the design shipped: the
> FMSG body IS a v6 code, the raw format is deleted, and §4.4's budget is
> asserted again at 4p with ~4x margin (240 chars of 1,000). Finding 3's blocker
> was fixed on `main` by `9db2c8a` (v6 codes a pending good); the probe that found
> it now reads `good_mask lost 0`. **Finding 4 (MAX_LOGS) is the one open item,
> and it does not block v1** — see §6. Read §1-3 as the record of why the body is
> what it is, §4-7 as live.

---

## 0. TL;DR

1. **The raw body blows §4.4's size budget and always will** — not a bug, an
   information-theory result. Raw spends ~34 bits on an action worth ~1-2 bits.
2. **The fix already exists and is 13x better**: put a **v6 replay code** in the
   body. The handoff ruled v6 out for the wrong reason.
3. **But v6 cannot currently represent a mid-round `good`**, which is the single
   most common iMessage turn at 3+ players. **That is the one thing standing
   between us and the whole design.** Fixing it is a change to `replay.c`,
   in place on v6 (owner's call: NOT a new format 7).

   Measured over 4,579 mid-game cuts: **0 state mismatches** — everything else
   round-trips exactly — and `good_players_mask` lost on **47% of 4p cuts**. The
   blocker is real, and it is exactly one missing atom wide.

Decisions already taken by the owner: **drop the raw format entirely** (too big),
**fix v6 in place**, **do not invent v7**. Work paused before touching `replay.c`
because other codec work is in flight.

---

## 1. Finding 1 — the raw body misses the size budget by 1.33x, permanently

`IMESSAGE_GAME_DESIGN.md` §4.4 asserts *"P95 full-game envelope < 1,000 base32
chars at 4 players"*. With the body as raw seat-prefixed awire frames
(handoff §3.2), measured:

| players | median | **P95** | vs the 1,000-char budget |
| --- | --- | --- | --- |
| 2p | 592 ch | **792 ch** | passes |
| 4p | 1,040 ch | **1,328 ch** | **1.33x OVER** |
| 8p | 2,104 ch | **2,904 ch** | 2.9x over |

**The spec's estimate was not wrong about 2p — it was wrong to extrapolate.**
§4.4 derived the budget from a heads-up game of "~60-90 actions". Measured 2p is
**71 actions** — dead in that range. But a 4p game runs **~150 actions** and 8p
runs **~346**, because every extra attacker adds actions per round. The 1,000
was then asserted *at 4 players* without measuring there.

This is not fixable by tuning. A raw awire frame costs ~4.3 bytes (~34 bits): a
seat byte, a kind byte, an n byte, a card byte. The *information* in an action is
its index into that state's legal-move menu — menus are small, so ~1-2 bits.
**The raw body is ~20x the entropy of what it carries.** No budget survives that.

Note it is not a *functional* problem: Apple's documented `MSMessage.url` cap is
5,000 chars (handoff §3.5), so even 8p sits at ~58% of the platform limit. The
1,000 is a self-imposed target. But the owner's instinct was right —
*"we were never seeing replay codes so large EVER"*.

## 2. Finding 2 — the codec already does this 13x better, and we should just use it

A full game's **v6 replay code averages 54.5 B across np=2..8**
(`./build/replay_v6_test 30`). The same games, the same information, as a raw
FMSG chain: 585 B at 4p. Measured head-to-head on identical games:

| players | raw chain | **v6 body** | ratio | envelope total | vs budget |
| --- | --- | --- | --- | --- | --- |
| 2p | 288 B | **34 B** | 8.4x | ~119 B ≈ **192 ch** | 19% |
| 4p | 585 B | **45 B** | 13.0x | ~130 B ≈ **208 ch** | **21%** |
| 8p | 1,210 B | **68 B** | 17.7x | ~153 B ≈ **248 ch** | 25% |

**Confirmed after shipping** — whole sealed envelopes, `robusta`, P95: 2p **192
ch**, 4p **240 ch**, 8p **328 ch**. The estimates held.

So the v6 body takes 4p from **1.33x over budget to 5x under it**, with no new
coder: `replay.c` already entropy-codes each action against the legal-move menu,
is production-proven at ~787k assertions, is version-dispatched, and already
supports a **mid-game cut** (explicit atom count) — which is exactly what a turn
bubble is.

### Why the handoff ruled v6 out, and why that reason doesn't apply

`IMESSAGE_IMPLEMENTATION_HANDOFF.md` §3.3 says v6 "is NOT the iMessage
continuation format" because it "deliberately carries no deal seed": a mid-game
v6 stream reveals the cards dealt SO FAR but says nothing about the undealt
stock, so two devices cannot draw identically from it.

**That is true of v6 *alone*. It is not true of v6 inside this envelope.** The
FMSG header already carries the 32-byte seed. Seed (the future) + v6 code (the
past) is precisely the pair serverless play needs. The handoff's own conclusion —
"FMSG v1 = seed + action chain stands" — silently assumed "action chain" meant
*raw frames*; it never had to. §16's "FMSG v2 = seed in header + rANS-coded
actions against legal-move menus" is a description of a thing that already ships.

The v6 body's reveals are redundant once the seed is present (~7% of the code, ~3
B at 4p). Not worth a leaner seeded variant — 95% of the win for 0% of the risk.

## 3. Finding 3 — THE BLOCKER: v6 cannot encode a mid-round `good`

**This is the one that matters and the reason work is paused.**

The replay codec does not model an individual `good` declaration. Evidence in
`cnitro/src/replay.c`:

- `:930` — an action stream atom is only emitted for `LOG_ATTACK / LOG_COVER /
  LOG_PASS / LOG_PICKUP`, plus a `round_end` marker synthesized when a
  `LOG_DISCARD` is *directly preceded by* a `LOG_GOOD`. **Individual
  `LOG_GOOD`s are dropped by the encoder.**
- `:628-630` — `apply_round_end()` *re-synthesizes* a `LOG_GOOD` for every
  non-defender IN player at decode time.

So a round that **completed** round-trips exactly: `good(s1), good(s2), DISCARD`
encodes as one `round_end` atom, and decode re-emits both goods. That is why
`replay_v6_test` is green and why full-game encoding measured fine.

**But a chain that ends mid-round with goods pending has no representation.**
Sequence `attack, cover, good(seat1)` — seat 1 declares "good" and sends the
bubble — encodes as `attack, cover`. The `good` is silently lost:

- the receiver's `good_players_mask` is 0 where the sender's was `1<<1`;
- seat 1's declaration vanishes and must be repeated;
- worse, the round can **stall**: the transition fires only when *all* attackers
  are good, so a dropped earlier good means the last attacker's good never
  triggers `execute_round_transition`.

`good(seat)` mid-round is the **most common turn in a 3+ player iMessage game**.
This is not an edge case; it is the main line.

### Measured — and the gap is EXACTLY this, nothing else

`probe_v6_midgame` (in `msg_wire_test.c`) encodes the game at **every mid-game
cut**, decodes, re-applies the decoded logs, and diffs the result against the
truth:

```
v6mid np=2: 2179 cuts | enc_fail 0 | dec_fail 0 | STATE MISMATCH 0 | good_mask lost  141
v6mid np=4: 2400 cuts | enc_fail 0 | dec_fail 0 | STATE MISMATCH 0 | good_mask lost 1124
```

Read that carefully, because it is better news than it looks:

- **`STATE MISMATCH 0` over 4,579 mid-game cuts.** Hands, battles, defender,
  deck count — every one of them round-trips *exactly*, at *every* cut point, at
  2p and 4p. v6-as-body is sound; the mid-game cut works.
- **The one and only divergence is `good_players_mask`** — the pending goods.
  **141/2179 = 6.5% of 2p cuts, and 1,124/2,400 = 47% of 4p cuts.** Nearly half
  of all mid-game 4-player states are unrepresentable today.

So the blocker is real and it is *narrow*: one missing atom, not a structural
problem with using v6. Fix the good atom and the whole design lands.

### The shape of the fix (owner: in place on v6, not a v7)

The codec needs an atom for a pending `good`. Sketch, to be designed properly:

- add a `good(seat)` option to the menu built in `replay.c`'s `calc_options`
  (alongside `OPT_ROUND_END` at `:751`), coded like any other menu index — it
  costs ~1-2 bits only in states where a good is actually legal;
- stop dropping `LOG_GOOD` in the encode-input builder (`:921-931`), while
  keeping the `GOOD+DISCARD -> round_end` synthesis so **completed rounds encode
  exactly as they do today**;
- `apply_round_end` must then not double-emit goods it has already seen.

**v5 must stay byte-frozen** (`replay.h`: existing v5 integers in
`game_snapshots.moves` must decode byte-identically). Whether v6 codes already
exist in the wild decides if this is a compatible extension or a re-cut of v6 —
the server's finalize emits v6 (`A4`), so **check `game_snapshots` before
changing v6's coding**. That question is unresolved and is the first thing to
settle.

## 4. Finding 4 — `replay_encode_v6_from_game` refuses ~10% of full 8p games

`replay_encode_v6_from_game` reads actions out of `g->logs` and **rejects a game
whose log buffer overflowed** (`num_logs >= MAX_LOGS`; `replay.h`). Measured:
**25/240 full 8p games failed to encode**, hitting `MAX_LOGS=512` exactly. 2p and
4p: **0 failures** (max observed 176 and 335 logs).

The owner's steer is to fix this rather than keep a raw fallback for it. Options,
unevaluated:

- **The action list need not come from logs at all.** FMSG *has* the chain — the
  device decoded the parent body and appended its own action. The only reason
  `from_game` touches `g->logs` is to recover the action list. A sibling entry
  that takes the action list directly (`replay_encode_v6_from_actions(seed,
  n_players, actions...)`, with `from_game` refactored to call it) sidesteps
  `MAX_LOGS` entirely and keeps ONE producer. **This looks like the right fix**
  and it composes with the finding-3 work.
- Raise `MAX_LOGS` on the encoding hosts only (iOS `libfoolish.a`, `bots.wasm`).
  Cheap, but `rules.wasm` cannot follow (pinned 3-page memory, `MAX_LOGS=128`),
  and it only moves the ceiling.
- Note the terminal state may never need an FMSG encode at all: game end ships a
  standard `foolish.cards/<code>` replay link (§12 / M4), not an FMSG payload —
  and mid-game states carry fewer logs than the full games measured here. Worth
  confirming before over-building.

## 5. What shipped

`claude/imessage-m0`. `./build/msg_wire_test 20` green; `rules.wasm` links at
36,626 B (inside its pinned 3 pages).

- `cnitro/src/sha256.{h,c}` — FIPS 180-4, freestanding. No cryptographic hash
  existed in-tree (the FNV mixers are seeds, not commitments); `parent8` and Rule
  P's tiebreak need one identical on every device. KAT-pinned.
- `cnitro/src/awire.{h,c}` — `awire_frame_len` + `awire_encode`, both onto one
  shared head check. (The raw body is gone, but these stand on their own: only a
  decoder existed, because the browser was the only producer.)
- `cnitro/src/msg_wire.{h,c}` — the envelope. `msg_seal` is the producer,
  `msg_decode` is structure, `msg_replay` is semantics.
- `cnitro/tests/msg_wire_test.c` — in `make difftests`, ahead of
  `solver_difftest` (which fails identically on `main` and would otherwise stop
  it ever running — `NEXT_STEPS.md` §5).

### The four things worth not re-deriving

1. **A continuation is not a replay, and that is what the seed is for.**
   `replay_steps.c` rebuilds a Game from a code's DEAL/DRAW atoms and fills the
   never-drawn tail in **canonical order** — right for rendering a finished game
   ("its identities do not [matter] — nothing reveals them", `rs_build_deck`),
   **wrong to play on from**: a continuation draws from that tail, and canonical
   order is not the shuffled stock. So `msg_replay` deals the TRUE deck from the
   seed and takes only the ACTIONS from the code. Seed = the future, code = the
   past. (`replay_steps.h` advertises its last game as what "a continuation (an
   iMessage turn) plays on from" — true of the POSITION, not of the stock.)
2. **`turn` counts ATOMS, and atoms are not moves.** The codec folds a bout's
   closing goods into one `round_end`, so 8 kernel actions can be 5 atoms. Only
   the codec knows the number, and Rule P orders chains on it. `msg_seal` derives
   `n_actions`/`turn`/`round` by decoding the body it just wrote — so a host
   cannot emit a payload it would itself refuse.
3. **Round counting must not read logs.** It watches `num_battles` fall to 0, and
   there are **three** closure paths: `handle_pickup`, the all-good
   `execute_round_transition`, and **a cover that empties the defender's hand**
   (`game.c:689`, discards inline). Counting `LOG_DISCARD` would be wrong in
   `rules.wasm`, which builds at `MAX_LOGS=128` and **silently drops** overflow.
4. **The tamper matrix asserts canonicality, not rejection.** A surviving
   bit-flip is not a break (integrity is the digest checked against `parent8`);
   what matters is that anything which decodes **re-encodes to exactly its own
   bytes**, or two payloads share one digest and `parent8` stops identifying a
   unique parent. Likewise **truncation is not a decode-time verdict** any more:
   cutting an entropy-coded body leaves a structurally perfect envelope, and a
   short code is just a different code. The header it no longer matches is what
   catches it — so the test asserts decode+replay, which is what "validation =
   replay" always meant.

## 6. M0 is done — what shipped, and the one thing that did not

Every M0 deliverable in `IMESSAGE_IMPLEMENTATION_HANDOFF.md` §4 is landed and
green: the envelope + its native suite (in `make difftests`), the wasm exports +
TS bridge, `e2e/msg_wire.test.ts`, `e2e/msg_concurrency.test.ts`, and
`/m/[payload]`. 22/22 e2e across the three iMessage suites, 509 native units,
`msg_wire_test` OK, regressions green.

**Two deliberate departures from the handoff, both toward less code:**

1. **The body is a v6 code, not raw frames** (§1-3 above).
2. **Rule P and Rule R are in C, so the M3 Swift port is cancelled.** The handoff
   wanted them as pure TS, "the reference implementation the Swift port must
   match fixture-for-fixture". But Rule P decides which game every player SEES —
   a phone and a browser disagreeing forks it — and Rule R's guard is a rules
   question, which §17.16 forbids Swift from answering. A second implementation
   to keep in step is the risk, not the deliverable. `e2e/msg_concurrency.test.ts`
   is now the model's TEST rather than its home.

**The seed rider (A7/F9)** is satisfied structurally: the wire field is a fixed
32 bytes and `MsgEnvelope.seed` is `uint8_t[32]`, so a short seed is
unrepresentable; `msg_seal` hands `MSG_SEED_LEN` to a producer that rejects
anything under `FOOLISH_SEED_LEN`; an all-zero seed is refused at encode AND
decode (pinned in both suites). What remains is kernel-side and belongs to A7:
`game_set_deal_seed_bytes` still degrades SILENTLY to the legacy LCG on a short
seed rather than saying so.

### The one open item: MAX_LOGS (finding 4)

`replay_encode_v6_from_game` refuses a game whose log buffer overflowed
(`num_logs >= MAX_LOGS` — a truncated stream is untrusted, `replay.c:1732`).
Measured: **0/240 at 2p and 4p** (max observed 176 and 335 of 512), **~10% of
full 8-player games** (512/512).

It does not bite v1: the UI caps at 4 players, where the ceiling is 65% clear,
and a FINISHED game ships a standard `foolish.cards/<code>` replay link rather
than an envelope (§12/M4), so the longest state never needs sealing.

Two ways out, neither started:

- **Feed the encoder the action list instead of scraping `g->logs`.** Looks
  right, and it is more work than it sounds: the encoder's `Src` walks LOG
  RECORDS, not actions, so "pass the actions" means synthesizing the log stream
  the atom mapper reads. It is a `replay.c` change and should ride whoever owns
  that file next.
- **Raise `MAX_LOGS` on the encoding hosts.** Rejected on inspection: 128 -> 512
  already took the `Game` struct 33KB -> 133KB, and cordite memcpy-clones a Game
  in its Monte-Carlo hot loop. Doubling it again to fix 8p would tax every bot
  decision in the product to serve a player count v1 does not ship.

## 7. The wasm half

- `wasm_msg_decode` / `wasm_msg_seal` / `wasm_msg_rule_p` / `wasm_msg_rebase`,
  exported from **bots.wasm only** — one big module behind every host, by steer.
  Not a per-host split: seal reads a resident session log rules.wasm cannot hold,
  and its scratch `Game` would blow that module's 3-page pin at link time.
- **The browser reaches the big module** through a base64 twin (`bots_wasm.ts`),
  emitted from the same Makefile rule as the static `.gz` off the same
  `build/bots.wasm`, so the two cannot drift. `bots.ts` picks its door by whether
  a filesystem exists. This was the one-big-wasm follow-up, done for the web.
- **LANDMINE: the envelope lives in `g_replay_io`, never `g_io`.** The rules build
  aliases the replay scratch family over the action family because "the two never
  nest" — an FMSG call IS a replay call, so an envelope in `g_io` would be
  clobbered by the codec's own bignum scratch mid-decode, and `MsgEnvelope`
  borrows those bytes.
- Decode leaves the game **resident**: `kernelMsgLegalMoves` and
  `kernelMsgPublicView` read it straight from the kernel. An iMessage device does
  not hold the game as a TS object — the envelope put it in the kernel and that
  is the only copy.
- `e2e/msg_wire.test.ts` is **the cross-engine gate** (design §8.2): fixtures
  sealed by the NATIVE kernel, decoded by the WASM one, agreeing on
  turn/round/seats/seed. Since `msg_replay` only returns when the body backs the
  header, agreement means both replayed the identical chain. A diff is a wire
  break.
- `rules_wasm.ts` was regenerated — stale since Format 6, so the browser/e2e
  kernel was missing A2-A5. That drift is paid off.
