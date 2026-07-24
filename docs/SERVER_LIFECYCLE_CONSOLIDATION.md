# The lifecycle skin — lobby/result/scoring kernel-reuse audit

*Investigation, July 2026 — a follow-on to `C_CORE_CONSOLIDATION.md`. That doc
(F1–F9) traced the TS server and the iOS offline client and moved the game's
INSIDE — rules, deal, legality, apply, per-seat masking, the bot cycle, the
replay codec, the bot roster — into the one C kernel. Since then a THIRD host
shipped: the native C server (`server/impls/native/foolish_server.c`), a
dedicated-process C backend built for scale. This audit asks the same
"do-not-repeat-yourself" question of the surface `C_CORE_CONSOLIDATION.md` did
NOT cover — the lifecycle skin AROUND a dealt game (lobby, seating, ready,
start, rematch, end-of-game result, scoring) — across all three hosts that now
implement it: the native C server, the Supabase/TS adapter
(`server/impls/supabase/functions/_shared/adapter/*`), and the game's SQL
(`commit_game` / `seed.sql`).*

*No code has been changed; this is the report + work order. The kernel is under
concurrent modification (animation work) — every proposed signature below is a
PROPOSAL to reconcile against the kernel's state at implementation time, and this
doc deliberately touches no `c/src/` file so it cannot conflict.*

*Companions: `C_CORE_CONSOLIDATION.md` (the F1–F9 program this extends),
`ARCHITECTURE_AS_A_PATTERN.md` (the doctrine), `RULES_DUPLICATION_FINDINGS.md`.*

---

## 0. Executive summary

`C_CORE_CONSOLIDATION.md` consolidated the game's interior and even built the
rematch reset as a kernel transform (F6 — `game_reset_to_lobby`). But the
**lobby/lifecycle state machine that wraps a game** was never given a kernel
home, and it is now reimplemented up to three times: TS adapter
(`meta_actions.ts`), SQL (`commit_game` CASE logic), and — newest — the native
C server (`h_meta` / `h_create`). The native server, being last to arrive, both
re-duplicated this skin AND regressed one piece the kernel had already fixed.

Findings, ranked by leverage (shared-across-hosts × no-kernel-home × bug-risk).
"Dup in" uses **N** = native C server, **T** = TS/Supabase adapter, **S** = SQL.

| # | Finding | Dup in | Verdict |
|---|---|---|---|
| L1 | **Lobby/seating state machine** — join (seat + human kind), add-bot (seat cap + roster pick + kind), set-ready, the `all-ready ∧ ≥2 → deal` start predicate, exit | **N + T** | **NEW.** No kernel entry exists; both servers hand-roll it, with a byte-identical start predicate. Propose a kernel lobby module (§4.1). |
| L2 | **Rematch reset** — `game_over → waiting` board clear | **N** | **REGRESSION.** F6's `game_reset_to_lobby` exists and is used by web/iOS; the native server calls it **0 times** and hand-rolls a PARTIAL reset that leaves stale `good`/board state in the lobby — the exact bug F6 fixed (§4.2). One-line fix. |
| L3 | **End-of-game result** — winner / fool / placements from `elimination_order` | **N + T** | Kernel has `game_done` (fool only); hosts recompute the rest. Add `game_result` (§4.3). |
| L4 | **"Does this game need a bot to act?"** predicate | **N + T** | Kernel already has the pieces (`bot_drive_eligible_mask` + `game_done`); expose one convenience wrapper (§4.4). |
| L5 | **Scoring math** — elimination→rankings, pairwise Elo delta, base-1000 | **T + S** | Three copies of one formula (+ the `main_elo.c` CLI). PURE and kernel-able, BUT the doctrine lists Elo as a host non-goal — a boundary call, not a slam dunk (§4.5). |
| L6 | **Initial lobby construction** — seat-0 / WAITING / empty-board literal | **N + T** | Folds into L1 as `game_create_lobby` (§4.6). |
| L7 | **Supabase-internal smears** — round-epoch rule across SQL+2×TS; bot roster rows mirroring `bot_roster.c`; move-kind dispatch tables; the F8 TS rules projections | **T + S** | Not shared with the C server; listed for completeness + hygiene (§4.7). |

Everything INSIDE a dealt game is already single-sourced (§1). The native C
server is, to its credit, a heavy and correct kernel consumer — the gaps below
are specifically the lifecycle skin, which no host's consolidation ever claimed.

## 1. Ground truth: what the native C server already reuses

Measured in `foolish_server.c` (call counts):

- `game_seat_and_deal` ×5, `awire_apply` ×11, `bot_drive` ×14, `state_put` ×24,
  plus `bot_roster_*` and `bot_pacing_ms`. So the deal, move validation+apply,
  the bot cycle (fairness/bundling/pacing), per-seat masking and the bot roster
  are all the kernel's — exactly as F1–F5 intend. The native server is a thin
  shell over the same machine code the wasm and iOS builds run.
- The semantic anti-cheat fuzzer (`sem_fuzz.c`) proves the one chokepoint this
  relies on — `awire_apply` — cannot be cheated regardless of transport.

What it does NOT call, and should: `game_reset_to_lobby` ×0 (→ L2),
`game_done` ×0 (→ L3). And what has no kernel entry to call at all: the lobby
join/add-bot/ready/start machine (→ L1).

## 2. Method

For every responsibility in the lifecycle skin, classify per host: **[K]**
already kernel, **[D]** duplicated host logic that belongs in C, **[P]**
platform-inherent (I/O, storage, identity, concurrency). The [D]s are §0's
findings. Sourcing note on confidence: the native-server findings (L1 seat
machinery, L2 reset bug, the start predicate, `seat_ready[]` duplication) and
the kernel surface were verified by direct read; the Supabase/SQL-side detail in
L5/L7 came from a survey pass and should be re-read before action.

## 3. The motivating bug (found by this audit — a live divergence)

**The native server's rematch leaves stale board state on the lobby screen.**
`h_meta`'s `continue` branch (`foolish_server.c:1524-1534`) hand-rolls the reset:

```c
} else if (!strcmp(type, "continue")) {
    g->status = GAME_STATUS_WAITING;
    for (int i = 0; i < g->num_players; i++) {
        bool ai = seat_is_bot(g, i);
        s->seat_ready[i] = ai;
        g->players[i].status = ai ? PLAYER_STATUS_READY : PLAYER_STATUS_IDLE;
    }
}
```

It sets status + per-seat status and nothing else — no clearing of `hand_count`,
`deck_count`, `discard_pile_length`, `has_flipped`, `num_battles`,
`num_eliminated`, `good_players_mask`, or `has_good_timestamp`. It "leans on the
next `start_game` to clear them." The kernel's `game_reset_to_lobby`
(`c/src/game.c:527-553`) clears ALL of them, and its own comment records WHY that
is the settled behavior:

> *The server's handleContinue left these two set and leaned on the next
> start_game to clear them; the web client's mirror cleared them here. The client
> was right — between the reset and the deal the lobby is on screen, and stale
> good state is visible in it. One definition, so the divergence is settled
> rather than mirrored.*

So the native server reproduced, by hand, the precise pre-F6 bug the kernel
already retired for web and iOS. Between a `continue` and the next `start`, a
native-server lobby can show stale "good" ticks and leftover board counts. L2 is
both a dedup and a bug fix.

## 4. Findings in detail

### 4.1 L1 — the lobby/seating state machine becomes a kernel module

**Where it's duplicated.** Both servers implement the same four transitions with
no kernel call between them:

| transition | native | TS/Supabase |
|---|---|---|
| join (seat append, human kind) | `h_meta` join branch | `meta_actions.ts handleJoin` |
| add-bot (seat cap, roster pick, kind, auto-ready) | `h_meta` add-bot branch | `handleAddBot` |
| set-ready + start predicate | `h_meta` start branch | `handleStart` |
| exit (remove seat, delete-if-empty) | reaper / host | `handleExit` |

The start predicate is not merely similar, it is the same rule:

- native (`foolish_server.c:1508-1510`): `all = num_players >= 2; for (i) if (!seat_is_bot(g,i) && !s->seat_ready[i]) all = false;`
- TS (`meta_actions.ts:56`): `players.every(p => p.status === PLAYER_STATUS.READY) && players.length >= 2`

And the native server keeps a `seat_ready[MAX_PLAYERS]` array in its `GameSlot`
that duplicates readiness the kernel ALREADY models in `player.status`
(`READY`/`IDLE`) — its own field comment concedes "host state; kind (human/bot)
lives in the kernel's strategy_key," but the readiness bit does not need to.

**Proposed kernel API** (identity stays host — the kernel touches only
count/kind/status; the host keeps `seat_user[]`/`seat_name[]` and maps by index):

```c
// Lobby transitions on a WAITING game. Identity is the caller's (see game.h:
// seat identity is deliberately not in the state blob). The kernel owns seat
// COUNT, per-seat KIND (strategy_key), readiness (player.status) and the start
// gate — the three things both servers currently hand-roll and can disagree on.
int  game_lobby_add_seat(Game *g, int8_t strategy_key);  // -> seat idx, or -1 if full / not WAITING
bool game_lobby_remove_seat(Game *g, int seat);          // lobby-only; compacts the roster
void game_lobby_set_ready(Game *g, int seat, bool ready);
bool game_lobby_should_start(const Game *g);             // ≥2 seated ∧ every human READY (bots always ready)
```

**What it removes.** Native: the `seat_ready[]` array and the join/add-bot/start
bookkeeping (readiness reads `player.status`); TS: the same handlers shrink to
identity + persistence around these calls. **The deal itself is already
`game_seat_and_deal` / `start_game_packed`** — L1 only owns getting TO the deal.

**Effort:** medium. **Risk:** low — it formalizes state the kernel already
half-owns. **Verify:** kernel invariants in `c/tests/tests.c` (add-seat respects
`MAX_PLAYERS` and WAITING-only; `should_start` matches the truth table; remove
compacts correctly), then byte-compare a lobby→deal sequence against the current
TS path before cutover, and re-point the native `h_meta` at the new calls.

### 4.2 L2 — adopt `game_reset_to_lobby` in the native server (bug fix)

F6 is built. The fix is to delete the six hand-rolled lines in §3 and call:

```c
game_reset_to_lobby(&s->game, bot_mask);   // bot_mask bit i = seat i is a bot
```

deriving `bot_mask` from `seat_is_bot(g,i)` over the seats (the same test the
loop already runs). This clears the full board, killing the stale-lobby
divergence. **Effort:** trivial. **Risk:** none (strictly a bug fix); guard with
a native smoke test that a `continue` leaves `good_players_mask == 0` and all
counts zero.

### 4.3 L3 — end-of-game result (winner / fool / placements)

The kernel exposes `game_done` (the fool seat, or -1). Hosts want more: TS
`handleContinue` and `check_win_sync` (`utils.ts`) recompute winner
(`elimination_order[0]`) and fool (still-`IN` seat) from the elimination order;
the native server surfaces status but not a ranking. One pure derivation:

```c
typedef struct { int8_t winner, fool; int8_t placements[MAX_PLAYERS]; } GameResult;
void game_result(const Game *g, GameResult *out);   // pure, from elimination_order + status
```

**Removes:** the winner/fool recomputation in TS; gives the native server a
result to report without adding its own copy. **Effort:** low. **Risk:** low.
Feeds L5.

### 4.4 L4 — one "needs a bot to act?" predicate

Supabase's `bot-heartbeat` uses `needsDriving = players.some(p => p.is_ai &&
p.status === 'in')` and `bot_actions.ts` a separate `in_players <= 1` stop; the
native `bot_thread` checks status inline. The kernel already has
`bot_drive_eligible_mask` and `game_done` — wrap them once:

```c
bool game_needs_bot_drive(const Game *g);   // PLAYING ∧ some eligible bot seat owes a move
```

**Effort:** trivial. **Risk:** none. All three hosts drop their ad-hoc versions.

### 4.5 L5 — scoring math (rankings → Elo delta) *(boundary call)*

Three copies of one formula: TS `calculateGameRankings` + `calculateEloChange`
(`common_utils.ts`) and `updateEloRatings` (`utils.ts`), the base-1000 constant
in SQL (`create_default_elo_rating`), and the standalone arena tool
`c/src/main_elo.c` (which has the pairwise math but is not a callable kernel
function). The pure parts are kernel-able:

```c
void game_rankings(const Game *g, int8_t placements_out[MAX_PLAYERS]);   // = L3's placements
void elo_apply(int32_t *ratings, const int8_t *placements, int n);        // pairwise, in place
```

**Caveat — this draws a new boundary line.** `C_CORE_CONSOLIDATION.md` §5 lists
"Elo and leaderboards (DB)" as an explicit host non-goal, and `game.h` says the
kernel models "only what the bots need." That non-goal is about *storage and
leaderboards* — the pure delta math is defensibly a rule — but moving it is an
owner decision, not an obvious win, and the native server does not score today
(so this is pre-emptive, not currently-shared). Lowest priority; needs sign-off.

### 4.6 L6 — initial lobby construction

`h_create` (native) and `create/index.ts` (Supabase) both build the seat-0 /
`WAITING` / empty-board literal. Fold into L1:

```c
void game_create_lobby(Game *g, int8_t creator_kind);   // seat 0 = creator, status WAITING
```

**Effort:** trivial (rides L1). **Risk:** none.

### 4.7 L7 — Supabase-internal smears (hygiene; not shared with the C server)

Listed for completeness — these duplicate WITHIN the TS/SQL host, not with the
native C server, so they are lower priority for THIS audit but real:

- **Round-boundary / stale-intent rule split three ways**: SQL `round_epoch`
  CASE in `commit_game` (`20260713120000_round_epoch_stale_guard.sql`) +
  TS `logwireHexClosesRound` → `p_closed_round` + TS `packed_action.ts`
  `intentVersion < roundEpoch` reject. One rule, three languages. Candidate:
  a kernel `game_round_closed(prev, next)` predicate. (The native server has no
  analogue — it uses in-process locks, not optimistic round epochs.)
- **Bot roster rows in `seed.sql`** are a hand-synced copy of `c/src/bot_roster.c`
  (the SQL comment says so). This is the tail of F1: generate the seed from the
  C roster instead of maintaining a parallel list.
- **Move-kind dispatch** duplicated in `action/index.ts` and `packed_action.ts`
  `dispatchLegacy` — mirrors of `awire_apply`.
- **F8 TS rules projections** (`canCover`, `game_done`, `get_next_player_index`,
  `shouldBotActCore`) + the deck-size constant `minValueFor` in `constants.ts`:
  already tracked as F8 cleanup. The native server uses the kernel for all of
  these, so this is TS-only debt.

## 5. Explicit non-goals (stays per-host, on purpose)

Unchanged from `C_CORE_CONSOLIDATION.md` §5, and reaffirmed for the native
server: identity (seat_user/seat_name/tokens), TLS/auth, the DB (Supabase) or the
in-memory store + SQLite write-behind (native), lease/CAS/version fences and
round-epoch concurrency, broadcast/push fan-out, rate limiting, timers/sleeps,
CPU budgeting, and all rendering. **The kernel decides what the lifecycle
transition IS; hosts decide how to persist it, guard it concurrently, and tell
the players.**

## 6. Action plan (ordered; standing playbook: propose → kernel invariant test →
cut over one host → byte/behavior-compare → cut over the rest)

| # | Action | When | Verification |
|---|---|---|---|
| LA1 | **L2 — native adopts `game_reset_to_lobby`.** Delete the hand-rolled `continue` reset; derive `bot_mask` from `seat_is_bot`. | now (independent, trivial) | native smoke: post-`continue`, `good_players_mask==0` + all board counts 0; existing `test.sh`/`tls_test.sh` still green |
| LA2 | **L1 + L6 + L3 + L4 — the lobby/lifecycle kernel module.** `game_create_lobby`, `game_lobby_add_seat/remove_seat/set_ready/should_start`, `game_result`, `game_needs_bot_drive`. Cut the native `h_meta`/`h_create` over first (drop `seat_ready[]`), then the TS `meta_actions.ts`. | after LA1, and after the concurrent kernel animation work lands (rebase these signatures onto it) | `c/tests/tests.c` invariants (mutation-checked): seat cap, WAITING-only, start truth table, remove-compaction, result from elimination_order; lobby→deal byte-compare TS-old vs kernel; native lobby smoke |
| LA3 | **L7 hygiene** — round-epoch predicate to the kernel; generate the `seed.sql` bot roster from `bot_roster.c`; fold the TS move-dispatch + F8 projections. | opportunistic | per-item; the SQL roster equals the C `seeded` set by construction, not by comment |
| LA4 | **L5 scoring** — ONLY after an owner decision on the Elo boundary (§4.5). `game_rankings` + `elo_apply`; storage stays host. | deferred; needs sign-off | pure-function tests + parity against the current TS formula on recorded games |

**What this buys:** the native C server sheds its `seat_ready[]` duplication and
its stale-lobby bug; the `all-ready ∧ ≥2` start rule and the winner/fool result
get one definition instead of two-to-three; a fourth backend (Telegram/Steam)
becomes "the same lifecycle calls" the way F2 made bots "the same three calls";
and "the lobby behaves the same on every server" becomes a compile-time property
rather than a hope — the same guarantee `C_CORE_CONSOLIDATION.md` won for the
game's interior, extended to its skin.

## 7. Status

Design only. No code changed; no `c/src/` file touched (the kernel is under
concurrent animation work — reconcile every signature here against it before
implementing). Nothing in §6 is started.
