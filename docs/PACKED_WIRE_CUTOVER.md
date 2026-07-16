# Packed wire cut-over — C buffers end to end

The state blob cut-over (docs/STATE_BLOB_CUTOVER.md) made the kernel's packed
blob the durable store. This change extends the same discipline to every hot
boundary: **game state now crosses client→server→client as kernel-produced
packed bytes**, and JavaScript objects exist only at the React render boundary.

```
DOM event ──JS──▶ action wire bytes ──guards.wasm validate──▶ POST (binary body)
   ──▶ edge fn: auth + seat lookup (TS) ──▶ rules.wasm:
         state blob in ─ apply action ─ finalize win ─ state blob out
         + per-viewer MASKED event streams (personalization in C)
   ──▶ CAS commit (blob) ──▶ realtime broadcast {v, s, b: base64(event wire)}
   ──▶ client: decode event wire ──▶ PersonalGame materialized per snapshot
   ──▶ React renders
```

The per-viewer **masking — "you only see your own hand" — is computed by the C
kernel** (`view.c`), not TypeScript: other players' hands and the deck leave
the kernel as `0xFE` hidden bytes (counts preserved). The TS server never holds
a JS `Game` object on the human-move path.

## Wire formats (all little-endian, 1-byte wire cards per c/wasm/wire.h)

### Action wire (`awire` v1) — c/src/awire.h
The bytes the client validates with guards.wasm are the exact bytes the server
kernel applies — one decoder (`awire_decode`), compiled into both modules.

```
u8 kind        0=attack 1=cover 2=pass 3=pickup 4=good
u8 n           card count (0 for pickup/good; 1..28 otherwise)
n  x u8        cards (cover: the covering cards)
n  x u8        cover only: the attack cards being covered (positional pairs)
```

HTTP request envelope (`POST /action`, `Content-Type: application/octet-stream`):
```
u8 REQ_FMT=1 | u8 gid_len | gid_len x ascii game id | awire...
```
HTTP response envelope (binary):
```
u8 RESP_FMT=1 | u8 status (0 applied, 1 rejected, 2 moot/game-over)
u8 reject_code (ENGINE_REJECT_*, 0 if n/a) | u32 committed version
```
JSON bodies on `action` still work (`bump`, legacy callers, tests).

### Masked view blob (`view` v1) — c/src/view.c
`wasm_view_serialize(viewer)`: `u8 VIEW_FMT=1 | u8 viewer_seat (0xFF spectator)`
followed by the put_state layout with masking: deck cards → `0xFE`, every hand
except the viewer's → `0xFE` per card (counts intact), other seats'
`awaiting_attack` → 0. Battles/flipped/statuses/masks are public and stay real.
`get_game` returns `u8 fmt | u8 flags | u8 my_seat | u32 version | u16 roster_len |
roster JSON (identity only: ids/names/is_ai + good order/timestamp + status) |
u16 view_len | view blob` when the request body carries `packed: true`.

### Event wire (`evwire` v1) — c/src/evwire.c
`wasm_events_serialize(viewer, actor, ended)` emits the whole animation
sequence for one recipient; a TS encoder (`sdk/ts/wire/evwire.ts`) produces
the byte-identical stream from JS `AnimationEvent[]` for the paths that still
run on JS Games (bot loop, meta/lobby) — parity-tested against the C output.

```
u8 EV_FMT=1 | u8 viewer_seat | u8 actor_seat (0xFF none) | u8 n_events
per event:
  u8 type      0 magic_transition, 1 deal, 2 flipped, 3 defender_move,
               4 attack_pass, 5 cover, 6 pickup, 7 discard, 8 out,
               9 refill, 10 cards_to_trash
  u8 seat      event player seat, 0xFF none
  u8 msg_code  0 none, 1 attacked, 2 passed, 3 out, 4 covered, 5 discarded,
               6 drew, 7 defender_move, 8 pickup, 9 good-transition,
               10 start_magic, 11 first-attacker-wait
               (strings are reconstructed client/test-side; the client UI
               never rendered server messages)
  u8 from_loc | u8 to_loc    0 deck, 1 hand, 2 table, 3 discard, 4 flipped, 0xFF none
  u8 flags     bit0 target_card, bit1 battle_index
  u8 n_cards | n x u8 cards  (DEAL/REFILL masked to 0xFE unless viewer==seat)
  [u8 target_card] [u8 battle_index]   (per flags)
  u16 snap_len | masked put_state bytes (this step's game_state, viewer-masked)
trailer:
  u16 final_len | masked put_state bytes (the committed final state)
```
Realtime payload: `{ t:'as2', s: sequence_id, v: version, b: base64(evwire) }`
on the same `gu-<game>-<player>` / `game-<game>` topics. A move that produces
zero events (a plain `good`) still broadcasts nothing — unchanged.

JS-encoded broadcasts (lobby/meta + bot moves) additionally carry
`r: { name, players: [{player_id, name, is_ai}] }` and `m: (string|null)[]`
— the roster, because join/exit/add-bot/rearrange are exactly the actions
that CHANGE it (a client's loaded roster is stale by definition when they
apply), and the original per-event message strings, because meta
MAGIC_TRANSITIONs carry arbitrary text the fixed message codes cannot
reconstruct. Kernel-encoded human moves never set them: a move cannot change
identities, and its messages rebuild from codes. A client receiving a packed
envelope for a game it has not loaded (no `r`, no local roster) refetches
`get_game` once and drops the sequence — the load lands a newer version.

## What runs where after the cut-over

- **rules.wasm** (server): `wasm_apply_action` (awire in, one call),
  `wasm_finalize_win(ai_mask)` (the old TS `check_win_sync`),
  `wasm_view_serialize`, `wasm_events_serialize`.
- **guards.wasm** (client): `wasm_validate_action` / `wasm_apply_action`
  (same awire bytes as the POST body), `wasm_import_view` (masked blob →
  resident state without a JS marshal).
- **TS server** (`_shared/packed_action.ts`): auth, user→seat via the roster
  column, CAS loop, the JSONB public dual for `commit_game` (`p_game`,
  heartbeat/lobby reads), per-move logs (DRAW identities hidden), end-game
  finalize (ELO/replay — materializes a JS Game exactly once, cold path).
- **TS client**: builds awire bytes from the DOM selection, decodes evwire and
  the view blob into `PersonalGame` **only at the render boundary**.

## The follow-up conversions (landed)

- **Session log = packed byte column** (`games.logs_packed`, "logwire"):
  kernel log records + u48 timestamps, DRAW identities masked IN the kernel
  (`wasm_export_logs_masked`), appended by hex concat inside the same
  version-fenced commit as the state blob (exactly-once by construction). A
  GAME_START batch resets the column; a WAITING commit clears both the blob
  and the log column (the `continue` reset must never leak the finished
  session's state into the new lobby). game_logs rows are write-never
  (legacy fallback read only). The end-of-game replay snapshot decodes the
  column — the one place session logs become JS objects.
- **Bot loop on the kernel**: every bot move applies via the same awire +
  packed pipeline (`executeBotMovePacked`; the choose-move marshal-skip makes
  the apply marshal free for kernel-brained bots), and `executeWithGameLock`
  accepts the kernel products directly (`PackedOpProducts`) — commit takes
  the blob + logwire hex, the broadcast takes the per-viewer buffers. No JS
  AnimationEvents or appendLogs on the loop. Legacy
  `processBotAction`/`executeBotMove` remain for offline harnesses/e2e.
- **Meta state-ops**: `handleStart`'s all-ready deal ships kernel products
  (`start_game_packed`) — the fattest broadcast goes kernel-native; add-bot's
  auto-start stays on the JS path deliberately (its broadcast must carry the
  roster extras, since the roster just changed). In-play `rearrange-hand` is
  validated and applied in the kernel (`wasm_rearrange_hand` — the
  permutation uniqueness check that prevents duplicate-card minting is C
  now). Pure roster I/O (join/exit/add-bot/update-name/continue) stays TS —
  it is membership plumbing, not game state.
- **`get_my_games` = packed list**: per game, the caller's kernel-masked
  view blob (+ roster JSON); lobbies/legacy rows ride as byte-wrapped
  personalize_game JSON in the same binary envelope
  (`encodeGamesList`/`decodePackedGamesList`).
- Still JS: the `hand_rearranged` private ack message, and the replay
  pipeline (already kernel-encoded at game end). guards.wasm now ships an
  explicit export allow-list like the other modules (25KB → 20KB); its
  `wasm_import_view` is exported and tested, though the client still
  marshals gates from the view model in this pass.

## What a move actually costs now

`e2e/bench_packed.ts` (per-move server COMPUTE, the part this cutover owns)
and `e2e/bench_e2e_move.ts` (full path against real Postgres). Note that
`bench_engine.ts` deliberately drives the LEGACY exports kept for offline
tooling — it does not measure this pipeline.

| per move, 4 players            | legacy JSON path | packed |
|--------------------------------|------------------|--------|
| server compute (apply + events + commit prep) | 45.9 µs | **14.8 µs (3.1×)** — kernel floor 9.7 µs |
| end-to-end w/ real local Postgres (load + kernel + CAS + broadcast) | 8.3 ms | **5.1 ms** |

The end-to-end residue is DB round trips, not compute. Two mitigations ship
here: the per-move load selects only the columns the path reads (the packed
session log GROWS all game and must never ride a state fetch), and a
CAS-fenced per-isolate cache (`game_cache.ts`) skips the load entirely when
this isolate wrote the previous state (the common case: consecutive human
moves and the bot loop the move scheduled). A stale cache can only cost one
conflict + reload — the version fence, not freshness, is what's trusted; a
kernel REJECT computed from cached state is retried against a fresh load
before it's surfaced (an apply self-corrects through the CAS; a reject never
reaches it). On production Supabase the cache removes one of the two
DB round trips per move (~1–15 ms each in-region — the dominant server cost;
local-socket Postgres understates it).

Perceived latency in the client is dominated by neither: ANIMATION_TIME is
500 ms per animation step by design. The engine is idle waiting for the
cards to fly.

## Closing the PostgREST side door

Masking in the kernel is worthless if the raw blob is readable elsewhere:
the `games.state` column holds the UNMASKED state, and the pre-existing
"Anyone can view games" RLS policy exposed it to any client via PostgREST.
Migration `20260707140000_hide_state_blob.sql` (mirrored in seed.sql)
switches `games` to column-level SELECT grants — everything except `state`
and the bot-lease columns. RLS cannot hide a column; grants can.

## Live verification checklist (needs a real Supabase stack; same drill as
STATE_BLOB_CUTOVER.md — the dev container cannot run edge functions)

1. Deploy `action`, `get_game` + the web client together (coordinated deploy:
   the broadcast format changes for all clients at server deploy).
2. Play attack/cover/pass/pickup/good/goods-transition through two browsers:
   animations play per step, each client sees only its own hand, spectator tab
   sees card backs for every DEAL/REFILL.
3. Reject path: force an illegal move (stale second client) → clean revert,
   reject code surfaced, no 500s.
4. Bot game: bot moves broadcast the packed format (TS encoder path) and
   interleave correctly with human packed moves.
5. Reload mid-game (get_game packed round-trip), dashboard list, replay share
   at game end.
