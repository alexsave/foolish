# Foolish backend protocol (client contract)

*The wire contract between a Foolish client and the Supabase backend. Milestone
D's first task (`IOS_APP_DESIGN.md` §15.1, §16.D0): the web client is the
protocol's only spec, so this doc extracts it from the verified anchors before
any Swift `Net/` code is written. **Status: draft skeleton** — the invoke/channel
shapes below are pinned from the §8.1 anchors; items marked `TODO(D0)` must be
confirmed by reading the named web files during Milestone D and reviewed (PR)
before `Net/` lands. Any surprise found later goes here first.*

Anchors (study before editing): `src/contexts/ServerContext.tsx`
(invokes `:423,633,1150,1180`; channels `:212,349,519,676`),
`src/state/RealtimeAnimationFeed.tsx:76`, `src/state/clientReconcile.ts`
(version gate `:44-52`), `supabase/functions/_shared/packed_action.ts` +
`wire/awire.ts`, `supabase/functions/_shared/player_views.ts`,
`supabase/functions/action/index.ts:17`.

---

## 1. Transport

All server calls are Supabase **Edge Functions** invoked via the Functions
client (`supabase.functions.invoke(name, { body })`). Realtime updates arrive
over Supabase **Realtime broadcast** channels. Auth is Supabase Auth (see §5).

Base config (client-supplied, both public by design — README "Quick start"):
`SUPABASE_URL`, `SUPABASE_KEY` (anon key). iOS keeps them in
`ios/Config/{Debug,Release}.xcconfig`, never in source (§16.D1).

## 2. Edge functions

| Function | Purpose | Body | Anchor |
| --- | --- | --- | --- |
| `create` | New game | JSON (game options) | `ServerContext.tsx:423` |
| `action` | ALL moves | **binary** packed action (§3) | `ServerContext.tsx:1180` |
| `meta`   | lobby / continue / fetch | JSON (generic) | `ServerContext.tsx:1150` |
| `add-bot`| add a bot seat | JSON | `supabase/functions/` listing |

`bump` nudge: the web fires an `action` of type `bump` at
`ServerContext.tsx:633` to prod bot turns. `TODO(D0)`: confirm whether `bump`
is a JSON `meta`/`action` call or a distinct packed kind (the awire wire has NO
bump kind — five kinds only: attack/cover/pass/pickup/good), i.e. bump is an
out-of-band nudge, not a packed move.

### 2.1 `action` request/response

**Request body** (packed, `wire/awire.ts` + envelope, ported in
`FoolishKit/Net/PackedAction.swift`):

```
[fmt=2][gid_len:u8][gid bytes][intent_version:u32 LE][ wire ]
  wire = [kind:u8][n:u8][card:u8 × n]   (+ [attackCard:u8 × n] for cover)
  kind: attack=0 cover=1 pass=2 pickup=3 good=4
  card byte = suit*13 + (value-1), 0..51 ; 0xFE hidden ; 0xFF none
```

`intent_version` is the client's intended `games.version` — the stale-round
guard (`WEB_RACE_BUG_HANDOFF.md` §5) compares it server-side. The iOS app sends
it from day one; it must not ship the old bug.

**Response** (7 bytes, `action/index.ts:17`):

```
[fmt=1][status:u8][reject_code:u8][version:u32 LE]
  status: applied=0 rejected=1 moot=2
  reject_code: kernel ENGINE_REJECT_* (0..21) ; edge REJECT_STALE_ROUND=100
```

## 3. Realtime channels

| Channel | Content | Anchor |
| --- | --- | --- |
| `pv-<user_id>` | personal view feed (your masked `PersonalGame`) | `ServerContext.tsx:349` |
| `game-<gameId>` | public / spectator view | `ServerContext.tsx:519,676` |
| `gu-<gameId>-<user_id>` | per-player animation event feed | `RealtimeAnimationFeed.tsx:76` |
| `chat:<gameId>` | chat | `ServerContext.tsx:212` |

`TODO(D0)`: pin the exact broadcast **event names** and payload JSON shapes for
each channel (the animation event list, the sequence wrapper).

## 4. Ordering & the version gate

Broadcasts arrive **unordered**. Every sequence carries the committed
`games.version`; drop stale sequences with the version gate
(`shouldDropStaleSequence`, `clientReconcile.ts:44-52`) — port it verbatim
(≈5 lines) to `Net/`. Each sequence carries the **full resulting** masked state
(`:47-49`), so Stage C1 rendering takes that state and feeds the board diff
(no optimistic mutation, §8.2).

## 5. Views (masking)

Clients receive per-viewer masked state (`player_views.ts`): the `PersonalGame`
JSON — your hand is real cards, other seats are counts, the deck is hidden. Wire
field names (web, `@shared/types.ts`):

```
PersonalGame = PublicGame + { self: PrivatePlayer }
PublicGame:  id, name, deck_length, discard_pile_length, flipped (Card|null),
             players: PublicPlayer[], status, power_suit, first_attacker,
             defender, table_battles: {attack, defense|null}[],
             elimination_order: string[], good_timestamp, good_players: string[],
             version?
PublicPlayer:  player_id, status ('idle'|'ready'|'in'|'out'), name, hand_length, is_ai
PrivatePlayer: PublicPlayer + { hand: Card[], awaiting_attack, strategy_key }
Card:          { suit: 0..3, value: 1..13 }   (hidden = {suit:-1, value:-1})
```

Note the web wire uses `suit`/`value` and **string** statuses; the offline
engine bridge (`cnitro/ios/ios_api.c`) uses the compact `{s,v}` + integer
statuses. `Net/` maps `PersonalGame` → the app's `GameView` at the boundary
(§16.D4), so the two representations meet in exactly one adapter.

## 6. Auth

Username+password over supabase-swift (session persisted in Keychain), mirroring
`AuthContext.tsx`:

- `nameToEmail(name)` = SHA-256(uppercased UTF-8 name) → first 16 hex chars →
  `<hex>@foolish.cards` (`AuthContext.tsx:12-33`; domain
  `WEBSITE_DOMAIN='foolish.cards'`).
- signUp carries `user_metadata.username` (uppercased) and must locally reject
  the bot-reserved prefix `%` anywhere in the name
  (`usernameUsesReservedPrefix`, `src/common/botName.ts`).
- Guest-first: online quick-match may prompt for a username only; no wall before
  play. Real-email verify/reset arrive with the web's auth rebuild (Oracle §4) —
  leave TODO seams.

## 7. Lifecycle & resync (§16.D6)

On foreground/reconnect: refetch authoritative state (`TODO(D0)`: the exact
`meta`/game-fetch call the web uses), resubscribe all channels, drop stale
sequences via the gate, rebuild the view. On websocket drop mid-game: a passive
"reconnecting…" banner; never block input on it (the POST path is independent of
the feed).
