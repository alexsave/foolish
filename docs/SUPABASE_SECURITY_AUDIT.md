# Supabase implementation — security audit

**Scope:** `server/impls/supabase/` (edge functions, migrations, RLS policies,
realtime authorization), with the web client (`src/`) consulted where it feeds
those endpoints.

**Frame:** audited against the 10 common "vibecoded app" security holes from the
`earlystartupdays` reel (IDOR, open RLS, client-side enforcement, missing rate
limiting, JWT forgery, RLS policy logic holes, public storage buckets, pre-auth
cost amplification, SSRF, prompt injection).

**Status:** findings only. No code has been changed. Fixes are proposed, not
applied.

**Bottom line:** this backend is well above the reel's baseline. Auth/JWT and
RLS+column-masking are done right, and there is no storage / SSRF / LLM attack
surface. There is **one authorization bug that is exploitable today** (`exit`),
**one expensive endpoint with no in-function authorization** (`bot-heartbeat`),
and **no rate limiting anywhere**. The rest are hardening.

---

## Scorecard

| # | Reel hole | Verdict |
|---|-----------|---------|
| 01 | IDOR — change a number, read/act on others' data | 🟡 Mostly solid — 1 real access-control bug (`exit`) |
| 02 | Supabase/Firebase DB readable by the public (RLS off) | 🟢 Strong — the headline hole is handled right |
| 03 | Price/permission decided in the browser (client-side enforcement) | 🟡 Server is authoritative — except `exit` trusts a client id; direct table INSERT open |
| 04 | No rate limiting on expensive endpoints | 🔴 No rate limiting anywhere |
| 05 | Forgeable JWTs / hand-rolled auth | 🟢 Excellent |
| 06 | RLS is ON but the policy has a logic hole | 🟢 No transitive hole — 2 notes |
| 07 | Anyone can LIST your storage bucket | 🟢 N/A — no storage surface |
| 08 | API bill on fire even with rate limiting (pre-auth / unlimited accounts) | 🔴 Real — `bot-heartbeat` has no in-function authz |
| 09 | Server fetches an attacker-supplied URL (SSRF) | 🟢 N/A |
| 10 | Chatbot talked into ignoring its instructions (prompt injection) | 🟢 N/A — no LLM in the product |

---

## Findings

### 🔴 Finding 1 — Broken access control in `exit` (items 01, 03) — exploitable today

`server/impls/supabase/functions/_shared/adapter/meta_actions.ts:137-178`
(`handleExit`) authenticates the request but performs **no authorization on the
caller**. It verifies the *target* is in the game, never that the *caller* is a
participant or is allowed to remove that target:

```ts
export async function handleExit({ user, body, game }: ExecutionParams) {
    let { bot_id, player_id } = body;
    ...
    if (bot_id) { /* removes bot — no caller check */ }
    else {
        if (player_id === undefined) player_id = user_id;
        verify_player_in_game(game, player_id);   // checks the VICTIM, not the caller
        game.players = game.players.filter(p => p.player_id !== player_id);
    }
    if (game.players.length === 0) { /* deletes the games + game_decks rows */ }
}
```

Every other meta handler (`handleStart`, `handleJoin`, `handleContinue`,
`handleRearrangePlayers`, `handleUpdateName`) calls
`verify_player_in_game(game, user_id)` on the **caller**. `handleExit` is the
outlier.

**Exploit.** Games are `SELECT`-public (item 02 intentionally allows lobby
listing), so any authenticated user can discover a `game_id` and then
`POST /functions/v1/meta`:

- `{"type":"exit","game_id":"…","player_id":"<someone-else>"}` → kick any player
  from any waiting lobby.
- `{"type":"exit","game_id":"…","bot_id":"<any-bot>"}` → strip bots from anyone's
  lobby.
- Repeat until the roster is empty → the handler **deletes another user's game**
  (`games` + `game_decks` rows dropped).

The client (`src/contexts/ServerContext.tsx:513`, `exitGame(gameId, botId?,
playerId?)`) sends `player_id` straight through — this is the reel's "the
browser decides, the user can change it." Limited to `WAITING` lobbies, so the
impact is griefing / denial, not data theft.

**Severity:** Medium.

**Proposed fix.** Require the caller to be a participant, and only allow removing
self or bots:

```ts
verify_player_in_game(game, user_id);                 // caller must be in the game
if (!bot_id && player_id !== undefined && player_id !== user_id)
    throw new Error('You can only remove yourself');
```

---

### 🔴 Finding 2 — `bot-heartbeat` runs expensive bot compute with no caller authorization (items 08, 04)

`server/impls/supabase/functions/bot-heartbeat/index.ts` drives the Monte-Carlo
bots (cordite, ~2s CPU per drive segment). Its entire auth story is a comment
(lines 13-15):

```ts
// Auth: the function relies on the platform's JWT verification — the cron and the
// self-dispatch both present the service-role key, which passes. We never call
// getAuthenticatedUser (there is no user here).
```

That reasoning is incorrect. The function sets no `verify_jwt` in `config.toml`
(the `[functions.bot-heartbeat]` block, lines 303-305), so it defaults to
`verify_jwt = true`. Platform JWT verification passes for **any valid project
JWT** — including the **public anon key embedded in the frontend** and any
logged-in user's token. It does **not** mean "service role only," and nothing
inside the function checks `role === 'service_role'`.

**Exploit.** Anyone holding the anon key (i.e. anyone who opens the web app) can:

- `POST /functions/v1/bot-heartbeat` with `{"game_id":"<any playing game>"}` →
  trigger a fresh ~2s-CPU `lockedBotLoop` on demand, in a loop (lines 52-58).
- `POST /functions/v1/bot-heartbeat` with `{}` (scan) → fan out drive requests to
  up to 100 games at once (lines 83-92).

This is the reel's "money pump" against Supabase edge compute: an expensive route
reachable without a real login, no per-user or per-IP limit (Finding 3), and
free/instant account creation makes per-user limits weak anyway. The `bump`
action on `meta` is a milder version (it at least routes through `wrap400`).

**Severity:** Medium-High.

**Proposed fix.** Authorize the caller *inside* the function: check the bearer
JWT's `role` claim is `service_role` (or require a shared secret header the cron
sets), and reject anon/authenticated tokens. Cheap and closes it completely.

---

### 🔴 Finding 3 — No rate limiting / usage cap anywhere (items 04, 08)

No rate limiting or usage cap exists on any edge function (no throttle/ratelimit
logic in `functions/`). No third-party *paid* API is called (the only
`openai_api_key` is Supabase Studio dashboard config, `config.toml:66`), so
there is no OpenAI-bill scenario — but the reel's principle still applies:

- `action` / `meta` schedule fire-and-forget bot loops (`scheduleBotLoop`) that
  run cordite (~127KB engine, budgeted up to ~2s CPU per move) on billable edge
  compute.
- `create` is unlimited: any user can mint unlimited games; `bump`
  (membership-free by design) can nudge loops; `bot-heartbeat` (Finding 2)
  amplifies this further.

**Severity:** Medium.

**Proposed fix.** Per-user and per-IP rate limits on `create` / `action` /
`meta` / `bot-heartbeat`, plus a hard cap on concurrent live games per user.
This is a design decision, not a mechanical fix — scope before building.

---

### 🟡 Finding 4 — `games` table is directly INSERT-able by any authenticated user (items 02, 03, 06)

`server/impls/supabase/seed.sql:285-288`:

```sql
CREATE POLICY "Authenticated users can create games" ON games
  FOR INSERT WITH CHECK ((select auth.role()) = 'authenticated');
```

With Supabase's default table-level grants, a client can `POST /rest/v1/games`
directly, bypassing the `create_game` RPC and writing any INSERT-able column
(including `state` / `game_seed`) on rows it creates. There is no UPDATE or
DELETE policy for client roles, so it cannot tamper with *other* users' rows —
impact is junk-row spam and feeding arbitrary blobs to the kernel deserializer on
its own rows.

This also underpins the item-06 note below: the edge functions authorize with
`verify_player_in_game(game.players, …)`, and `games.players` is a column the
user can set on a directly-inserted row.

**Severity:** Low-Medium.

**Proposed fix.** Drop the client INSERT policy and route all creation through
the `create_game` RPC (the app already does this via the `create` function).

---

### 🟡 Finding 5 — Realtime `user-` channels keyed by email prefix (item 02)

`server/impls/supabase/seed.sql:810-829`:

- `"authenticated can send private messages"` lets **any** authenticated user
  INSERT-broadcast to any `user-%` topic → spoofed "system notifications" to any
  user.
- The receive policy matches `user-<split_part(email,'@',1)>`, so users sharing
  an email local-part (e.g. `jane@gmail.com` vs `jane@yahoo.com`) **collide** and
  receive each other's private notifications.

These channels carry only system notifications today, so impact is low — but the
design is weak.

**Severity:** Low.

**Proposed fix.** Key the channel on `auth.uid()` (as the `gu-` game-user
channels already correctly do) and restrict sends to `service_role`.

---

### 🟡 Finding 6 — `user_elo_ratings` is readable by anon (item 06)

`server/impls/supabase/seed.sql:332-333`:

```sql
CREATE POLICY "ELO ratings access policy" ON user_elo_ratings
  FOR SELECT USING (true);
```

No role clause, so the policy applies to **anon** (unauthenticated public). The
table carries `user_id`, `username`, and `elo_rating`, so the entire
`user_id ↔ username ↔ elo` map is exposed to the whole internet, not just logged-in
users. Acceptable for a public leaderboard, but conscious of the exposure: it
publishes the `auth.uid ↔ username` mapping.

**Severity:** Low.

**Proposed fix (optional).** Restrict the SELECT policy to `authenticated`, or
serve the leaderboard through a view that omits `user_id`.

---

## What is done right (no action needed)

### 🟢 Item 05 — JWT / auth (excellent)

`server/impls/supabase/functions/_shared/adapter/auth.ts`:

- Local verification pins algorithms to **ES256/RS256 only**; `HS256/384/512` and
  `none` are rejected before any crypto runs (`isAlg`, line 226) — blocks
  alg-confusion and the "verify the public key as an HMAC secret" attack.
- Verification uses the platform's Web Crypto against the **public JWKS** — no
  secret key anywhere, never in the frontend.
- Algorithm-substitution bound: the key is imported under the header alg, so an
  EC key can't satisfy an RS256 header (lines 165-179).
- `exp` / `nbf` enforced with bounded skew (lines 255-258).
- `unverifiedSubFromToken` is explicitly documented as *not* authorization; every
  path still awaits real verification.

### 🟢 Item 02 — RLS + column masking (the reel's headline hole, handled right)

- RLS enabled on every table (`seed.sql:252-261`).
- Sensitive `games.state` (unmasked hands + deck order) and `games.game_seed` are
  `REVOKE`d and only safe columns re-`GRANT`ed (`20260707140000_hide_state_blob.sql`,
  `seed.sql:278-283`) — RLS can't hide a column, so column grants do.
- Hands / decks live in separate service-role-only tables (`game_decks`,
  `bot_hands`, `player_hands`).
- Personalized data is own-row only: `player_views` uses
  `USING (player_id = auth.uid())`. `spectator_views` is exposed to all
  authenticated users but is **fully masked** (seat -1, every hand a card-back).
- Writes go through `SECURITY DEFINER` RPCs (`commit_game` / `create_game`) with
  `SET search_path`; clients have no UPDATE/DELETE policy on game state.

### 🟢 Item 06 — RLS avoids the transitive-join trap

The classic hole ("policy on A joins to B, B is public") is deliberately avoided:
`chat_messages` and the realtime `gu-` / `chat:` policies join to `player_hands`
(own-row RLS, service-written), **not** to the user-settable `games.players`
JSONB (`seed.sql:314-321, 831-869`). `game_snapshots.player_ids` and
`player_views.player_id` are service-written columns the client cannot set. No
transitive leak found. (See Findings 4 and 6 for the two notes.)

### 🟢 Item 07 — no storage surface

Storage is `enabled` in `config.toml` but no buckets are defined and no code uses
the Storage API (`.upload`, `getPublicUrl`, `storage.from` — none). The app has
no user file uploads. Nothing to list.

### 🟢 Item 09 — no SSRF surface

The only server `fetch` of a non-constant URL is `bot-heartbeat`'s `SELF_URL`,
built from the `SUPABASE_URL` env var (not user input); the request body carries
only `game_id`. No "fetch a user-supplied URL" feature exists. The Oracle runs as
a client-side WASM worker — no server-side URL fetch.

### 🟢 Item 10 — no LLM / prompt-injection surface

The "Oracle" is a deterministic WASM Durak solver in a Web Worker
(`src/oracle/oracleWorker.ts`, `public/oracle.wasm.gz`) — not a chatbot. There is
no LLM, system prompt, or AI tool-calling in the product path. The only OpenAI
reference is Supabase Studio dashboard config (`config.toml:66`), which never
touches app data.

---

## Priority summary

| Pri | Finding | Item(s) | Fix size |
|-----|---------|---------|----------|
| 1 | `exit` has no caller authorization — kick/delete anyone's lobby | 01, 03 | tiny |
| 2 | `bot-heartbeat` has no service-role check — anyone triggers ~2s bot compute | 08, 04 | small |
| 3 | No rate limiting / usage cap anywhere | 04, 08 | design |
| 4 | `games` open INSERT policy — direct row writes bypass `create_game` | 02, 03, 06 | small (migration) |
| 5 | Realtime `user-` channels keyed by email prefix (spoof/collision) | 02 | small (migration) |
| 6 | `user_elo_ratings` readable by anon (uid↔username map public) | 06 | small (migration) |

Findings 1, 2, 4, 5, 6 are small, concrete fixes. Finding 3 (rate limiting) is
the one real design decision.
