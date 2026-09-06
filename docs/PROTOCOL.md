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
| `action` | ALL moves | **binary** packed action (§4) OR JSON `{game_id,type:'bump'}` | binary 7-byte response (§4) for a packed move; **binary** enveloped packed game (§5) for `bump` |
| `meta` | lobby ops | JSON `{type, game_id, …}` (§7) | **binary** enveloped packed game (octet-stream, §5) |

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

The `bump` nudge (`ServerContext.tsx:653`) is a JSON `action` body
`{ game_id, type: 'bump' }`, fired when the game has AI players.
It does not go through the packed branch, so it comes back through the generic
`wrap400` tail: the caller's enveloped packed game (§5), which every bump caller
discards - the nudge is the point, not the answer.

## 5. The enveloped packed game (RESOLVED byte layout)

`decodePackedGame` (`wire/view.ts:281`) / iOS `PackedGame`:

```
[0]      magic = GAME_RESP_FORMAT (1)
[1]      flags: bit0 = isPlayer, bit1 = a PACKED roster trailer follows
[2]      seat (when isPlayer) else -1
[3..6]   u32 LE version
[7..8]   u16 LE legacy roster JSON length (0 once the island is deleted)
[9..]    legacy roster JSON = PackedGameRoster { id, name, players:[{player_id,name,is_ai}], status, good_players, good_timestamp }
[q]      u16 LE view length          (q = 9 + rosterLen)
[q+2]    VIEW_FORMAT_VERSION (1)
[q+3]    viewer seat
[q+4..]  masked state — view.c state_put layout (writeMaskedState is its TS mirror)
[q+viewLen..]  PACKED roster (present iff flags bit1):
         u8 ROSTER_WIRE_FORMAT (1)
         u16 LE len + id | u16 LE len + name | u8 status (0 waiting, 1 playing, 2 game_over)
         u8 n, then n x { u8 seat, u8 name_len, name[] }      <- the kernel's own block
         n x { u16 LE id_len, player_id[], u8 is_ai }         <- same seats, same order
         u8 n_good, n_good x { u16 LE id_len, player_id[] }
         u8 has_ts, then f64 LE good_timestamp when has_ts
```

THE ROSTER IS PACKED, AND THE JSON ISLAND IS A COMPATIBILITY SHIM. Both are
written today. The reason the packed roster is a TRAILER rather than a
replacement is that merging a server PR here deploys instantly while the iOS
client ships through the App Store, and the same envelope is STORED (in
`player_views.view` / `spectator_views.view`) where there is no request to
negotiate a format on. Every shipped reader takes flags bit0 and ignores the
rest of the byte, and bounds the view blob without looking past it, so a
trailer plus a flag bit is invisible to all of them. The island is deleted in
one commit (`LEGACY_ROSTER_JSON` in `sdk/ts/wire/view.ts`) once no pre-trailer
client is in the field AND every stored row has been rewritten since the deploy.
The names block is the kernel's own (`fio_msg_decode_packed`'s tail,
`sdk/swift/RosterWire.swift`), so it is one codec and not a second.
`e2e/packed_roster_wire.test.ts` holds both halves: a frozen 1.0(43)
encoder/decoder pair, and the production Swift decoder compiled against the
production TypeScript encoder.

iOS decodes the whole envelope in pure Swift (`MaskedView`, the mirror of
`view.c state_put`; proven against kernel-emitted fixtures by
`ios/FoolishTests/PackedViewTests.swift` — the old `fio_view_from_packed_json`
C bridge and its `make ios-view-test` harness are retired), then merges the
roster's real names in (`sdk/swift/EnvelopeRoster.swift`). `player_views.
view` (hex) and the `create` response body are this same envelope.

## 6. Auth (RESOLVED)

`nameToEmail(name)` = SHA-256(uppercased UTF-8) → first 16 hex → `<hex>@foolish.
cards` (`Net/Auth.swift`, golden-tested). signUp carries `user_metadata.username`
(uppercased) and must locally reject the `%` prefix. supabase-swift persists the
session in the Keychain. Guest-first: no wall before play.

## 7. `meta` actions (RESOLVED)

`{type, game_id, …}` → the caller's **enveloped packed game** (§5), the same
bytes `create` returns, `player_views.view` stores and the realtime feed pushes.
The REQUEST stays JSON because it is a command, not game state; no game state
crosses this wire as JSON in either direction. A caller who left the game (or was
never in it) gets the seat -1 spectator envelope.

The web decodes it with `decodePackedGame` (`ServerContext.invokeGameFunctions`)
and reads `game.id`. iOS uses supabase-swift's `Void` `invoke(_:options:)`
overload, which never decodes a body, so it is unaffected. The seat is NOT
returned explicitly - the client reads it from the masked view (§5). Types
(`meta_actions.ts`):

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
