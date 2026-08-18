# Foolish backend protocol (client contract)

*The wire contract between a Foolish client and the Supabase backend, extracted
from the web client (the protocol's only spec) for the native iOS port
(`IOS_APP_DESIGN.md` §8, §16.D). Verified anchors 2026-07-14. This is now a
resolved contract, not a skeleton — the native `Net/` layer implements it. What
remains is END-TO-END testing against a live/staging backend (no wire unknowns).*

Anchors: `src/contexts/ServerContext.tsx` (create `:433`; channels `:228,355,
692`; meta `:462,1129`; action `:1192`; bump `:649`), `src/state/
RealtimeAnimationFeed.tsx:76`, `src/state/clientReconcile.ts:43-52`,
`sdk/ts/wire/view.ts` (`decodePackedGame:281`,
`encodeGameResponse:199`, `writeMaskedState`), `server/impls/supabase/functions/_shared/
meta_actions.ts`, `server/impls/supabase/functions/_shared/packed_action.ts`,
`server/impls/supabase/functions/action/index.ts:17`.

---

## 1. Transport

Edge functions via `supabase.functions.invoke(name, { body })`; realtime via
Supabase Realtime (both Postgres-change notifications AND broadcast — see §3).
Auth is Supabase Auth (§6). Config: `SUPABASE_URL` / `SUPABASE_KEY` (anon,
public), in `ios/Config/*.xcconfig`.

## 2. Edge functions

| Function | Purpose | Body | Response |
| --- | --- | --- | --- |
| `create` | new game | `{}` (JSON) | **binary** enveloped packed game (octet-stream, §5) |
| `action` | ALL moves | **binary** packed action (§4) OR JSON `{game_id,type:'bump'}` | binary 7-byte response (§4) |
| `meta` | lobby ops | JSON `{type, game_id, …}` (§7) | JSON `{ data: { id, … } }` |

### 2.1 create → game id + seat (RESOLVED)

`create` returns the creator's **enveloped packed game** as an octet-stream
(`create/index.ts` builds it via `buildPlayerViewRows`). The client decodes it
with `decodePackedGame` → `{ game, version, seat }`; **`game.id` is the game id
and `seat` is the creator's seat** (always 0 for create). No separate id/seat
call. iOS: `PackedGame.decode` (native mirror) → `DecodedGame{gameId, seat,
version, view}`.

## 3. Realtime channels (RESOLVED — note the transport per channel)

| Channel | Transport | Carries |
| --- | --- | --- |
| `pv-<user_id>` | **Postgres changes** on `player_views` (filter `player_id=eq.<uid>`) | the user's authoritative masked view — `row.view` is bare-hex of the §5 envelope; `row.version` is the committed version |
| `game-<gameId>` | Postgres changes on `spectator_views` (spectator) / broadcast `animation_events` | the public masked view / animation events |
| `gu-<gameId>-<user_id>` | **broadcast**, event `animation_events` | packed event envelope `{t:'as2', s, v, b}` — `b` is base64 masked EVENT bytes (animation polish, NOT a full view) |
| `chat:<gameId>` | broadcast, event `INSERT` | chat rows |

**Stage C1 (what the app renders from):** the `player_views` row — the FULL
resulting masked view, decoded per row (`applyRow` on the web). The
`animation_events` broadcast is animation-only and is deferred to Stage C2.
iOS `GameFeed` subscribes the `player_views` (or `spectator_views`) Postgres
changes and hands each `row.view` to `PackedGame`.

## 4. `action` (packed move) — RESOLVED, byte-verified

Request (`encodeActionRequest`, ported byte-for-byte in `Net/PackedAction.swift`
and verified against `awire.h`):

```
[fmt=2][gid_len:u8][gid bytes][intent_version:u32 LE][ wire ]
  wire = [kind:u8][n:u8][card:u8 × n]  (+ [attackCard:u8 × n] for cover)
  kind: attack=0 cover=1 pass=2 pickup=3 good=4 ; card = suit*13+(value-1)
```

`intent_version` = the client's current `games.version` (from the last decoded
envelope) — the stale-round guard (already live, `round_epoch_stale_guard.sql`)
rejects cross-round moves. Response (7 bytes, `action/index.ts:17`):

```
[fmt=1][status:u8][reject_code:u8][version:u32 LE]
  status: applied=0 rejected=1 moot=2 ; reject_code: ENGINE_REJECT_* (0..21), REJECT_STALE_ROUND=100
```

**Client must request the RAW body** (not JSON-decoded) for this binary response.

The `bump` nudge (`ServerContext.tsx:649`) is a JSON `action` body
`{ game_id, type: 'bump' }`, fired when the game has AI players.

## 5. The enveloped packed game (RESOLVED byte layout)

`decodePackedGame` (`wire/view.ts:281`) / iOS `PackedGame`:

```
[0]      magic = GAME_RESP_FORMAT (1)
[1]      flags: bit0 = isPlayer
[2]      seat (when isPlayer) else -1
[3..6]   u32 LE version
[7..8]   u16 LE roster JSON length
[9..]    roster JSON = PackedGameRoster { id, name, players:[{player_id,name,is_ai}], status, good_players, good_timestamp }
[q]      u16 LE view length          (q = 9 + rosterLen)
[q+2]    VIEW_FORMAT_VERSION (1)
[q+3]    viewer seat
[q+4..]  masked state — view.c state_put layout (writeMaskedState is its TS mirror)
```

iOS decodes the whole envelope in pure Swift (`MaskedView`, the mirror of
`view.c state_put`; proven against kernel-emitted fixtures by
`ios/FoolishTests/PackedViewTests.swift` — the old `fio_view_from_packed_json`
C bridge and its `make ios-view-test` harness are retired), then merges the
roster's real names in. `player_views.
view` (hex) and the `create` response body are this same envelope.

## 6. Auth (RESOLVED)

`nameToEmail(name)` = SHA-256(uppercased UTF-8) → first 16 hex → `<hex>@foolish.
cards` (`Net/Auth.swift`, golden-tested). signUp carries `user_metadata.username`
(uppercased) and must locally reject the `%` prefix. supabase-swift persists the
session in the Keychain. Guest-first: no wall before play.

## 7. `meta` actions (RESOLVED)

`{type, game_id, …}` → `{ data: { id, … } }`. The seat is NOT returned explicitly
— the client reads it from the masked view (§5). Types (`meta_actions.ts`):

- `join {game_id}` — appends the caller to `game.players` (seat = new array
  index); the `player_views` feed then delivers their masked view.
- `continue {game_id}` — rematch; resets a GAME_OVER game to WAITING (mirror
  `resetToLobby`, `clientReconcile.ts:10-40`).
- `start {game_id}` · `add-bot {game_id, bot_id?}` · `exit {game_id, bot_id?,
  player_id?}` · `update-name {game_id, new_name}` · `rearrange-hand
  {game_id, card_indices}` · `rearrange-players {game_id, new_order}`.

## 8. Ordering & resync

Version gate (`shouldDropStaleSequence`, ported in `Net/VersionGate.swift`): drop
any row/sequence whose `version` ≤ the newest applied. On foreground/reconnect:
reset the gate, re-`select` the `player_views` row (`OnlineGame.resync`), re-apply.

## 9. What remains (testing, not wire)

- End-to-end against a **staging** Supabase project (never prod): quick-match vs
  a bot, a 2-device human game, spectate, reject surfacing, resync.
- Confirm supabase-swift 2.x exact API shapes at compile time (see the
  `NOTE (Mac compile pass)` markers in `Net/`).
- Stage C2 (optional): decode the `animation_events` broadcast for smoother
  animation; the app is correct on `player_views` rows alone.
- Online replay code surfacing (minted server-side at game end).
