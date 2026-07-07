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

## Wire formats (all little-endian, 1-byte wire cards per cnitro/wasm/wire.h)

### Action wire (`awire` v1) — cnitro/src/awire.h
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

### Masked view blob (`view` v1) — cnitro/src/view.c
`wasm_view_serialize(viewer)`: `u8 VIEW_FMT=1 | u8 viewer_seat (0xFF spectator)`
followed by the put_state layout with masking: deck cards → `0xFE`, every hand
except the viewer's → `0xFE` per card (counts intact), other seats'
`awaiting_attack` → 0. Battles/flipped/statuses/masks are public and stay real.
`get_game` returns `u8 fmt | u8 flags | u8 my_seat | u32 version | u16 roster_len |
roster JSON (identity only: ids/names/is_ai + good order/timestamp + status) |
u16 view_len | view blob` when the request body carries `packed: true`.

### Event wire (`evwire` v1) — cnitro/src/evwire.c
`wasm_events_serialize(viewer, actor, ended)` emits the whole animation
sequence for one recipient; a TS encoder (`_shared/wire/evwire.ts`) produces
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

## Kept on the JS path (encode-at-the-edge, follow-ups)

- Bot loop and meta/lobby actions still mutate JS Games internally; their
  broadcasts are encoded to evwire by the TS encoder so the client sees one
  format. Converting the bot loop to resident-kernel apply is the next step.
- `get_my_games` (dashboard listing) still returns personalize_game JSON.
- `hand_rearranged` private message unchanged.
- Replay pipeline unchanged (already kernel-encoded at game end).

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
