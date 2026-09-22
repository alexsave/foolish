-- =============================================================================
-- Supabase Schema for Game Application
-- =============================================================================
-- THE WHOLE DATABASE, AND THE ONLY DEFINITION OF IT. Every database this project
-- runs on - a `supabase start`, an e2e test's own, the hosted project - is this
-- file. It drops what it finds and builds the schema, the RLS, the grants, the
-- kernel writers, the two pg_cron jobs and the bot roster from nothing, and it is
-- idempotent: running it again lands in the same place.
--
-- The `server/impls/supabase/migrations/` directory is gone. It held 39 files
-- recording how the hosted database got from June 2026 to here, every one of them
-- already applied there, and this file had been kept equal to their end state by
-- a test that built both and compared the catalogs object for object. A comment
-- below that says "migration 20260708120000" is therefore a citation of git
-- history, not a path: `git log -- server/impls/supabase/migrations` still has
-- every one of them, and they are worth reading for the reasoning. Nothing in
-- the tree reads them.
--
-- HOW A SCHEMA CHANGE REACHES PRODUCTION: hosted is the one database that cannot
-- be rebuilt from this file, since it drops every table it finds, so a change
-- gets there as a delta - written twice, in the same commit: here in its final
-- form, and in server/impls/supabase/migrations/ as the statement that takes
-- today's production database to it. That directory is normally empty, a merge
-- to main runs no SQL against hosted, and its README has the rule for the day
-- somebody needs one.
-- =============================================================================

-- Enable necessary extensions first
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- =============================================================================
-- CLEANUP: Drop existing objects in correct dependency order
-- =============================================================================

-- Drop functions first (triggers will be dropped automatically with tables)
DROP FUNCTION IF EXISTS update_updated_at_column() CASCADE;

-- Drop tables in reverse dependency order (this will automatically drop all policies and triggers)
DROP TABLE IF EXISTS spectator_views CASCADE;
DROP TABLE IF EXISTS player_views CASCADE;
DROP TABLE IF EXISTS game_snapshots CASCADE;
DROP TABLE IF EXISTS chat_messages CASCADE;
DROP TABLE IF EXISTS bot_hands CASCADE;
DROP TABLE IF EXISTS player_hands CASCADE;
DROP TABLE IF EXISTS games CASCADE;
DROP TABLE IF EXISTS user_elo_ratings CASCADE;
DROP TABLE IF EXISTS bots CASCADE;
DROP TABLE IF EXISTS auto_discard_locks CASCADE;

-- Drop custom types
DROP TYPE IF EXISTS game_status CASCADE;
DROP TYPE IF EXISTS player_status CASCADE;

-- =============================================================================
-- CUSTOM TYPES: Define enums for better type safety
-- =============================================================================

CREATE TYPE player_status AS ENUM (
  'idle',
  'ready', 
  'in',
  'out'
);

CREATE TYPE game_status AS ENUM (
  'waiting',
  'playing',
  'game_over'
);

-- (log_type enum retired with the game_logs table — the session log now lives
-- in games.logs_packed as packed bytes, whose record types are the C kernel's
-- LOG_* ids, not this SQL enum. See migration 20260708120000.)

-- =============================================================================
-- MAIN TABLES: Separated for security
-- =============================================================================

-- Games table - SENSITIVE: service_role only (no client privilege, no policy).
-- A row is the kernel's two durable blobs plus the scalars SQL filters and
-- fences on (docs/C_GAME_SHAPE_MIGRATION.md 3.1). Clients never read it: every
-- viewer reads its masked envelope from player_views / spectator_views.
CREATE TABLE games (
  id TEXT PRIMARY KEY,
  status game_status NOT NULL DEFAULT 'waiting', -- the blob's status, written from the kernel on every commit (Q6): for SQL filters and player_views.status
  state BYTEA NOT NULL, -- the kernel's durable board (c/src/table.h, v02): every hand and the deck order, a lobby included
  roster BYTEA NOT NULL, -- the kernel's durable roster (c/src/roster.h, format 1, 1227 bytes): each seat's id, name and bot brain, and the table title
  needs_bots BOOLEAN NOT NULL DEFAULT FALSE, -- PLAYING and a bot seat is still IN (the kernel's table_needs_bots): the bot heartbeat's scan predicate.
  game_seed TEXT, -- 64 hex chars = the 32-byte deal seed the deck was ChaCha-shuffled from. Regenerates the deal for audit/replay. NULL in a lobby.
  logs_packed BYTEA, -- the session log: kernel log records + u48 timestamps, DRAW identities pre-masked, appended by commit_table. Read by the bot loop and the replay snapshot; empty in a lobby and once the snapshot is stored.
  version BIGINT NOT NULL DEFAULT 0, -- optimistic-concurrency token (commit_table's version fence)
  round_epoch BIGINT NOT NULL DEFAULT 0, -- the `version` at which the CURRENT round began, stamped by commit_table's p_closed_round (pickup/discard). The kernel's stale-round guard refuses a move whose intent version predates it (TABLE_STALE_ROUND). 0 = round 1 / never closed.
  bot_lease_token UUID,              -- bot-loop lease holder token (replaces bot_locks)
  bot_lease_until TIMESTAMPTZ,       -- bot-loop lease expiry; auto-expiring, no finally-release needed
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW(),
  last_commit_at TIMESTAMPTZ NOT NULL DEFAULT now() -- now() at the moment `version` last changed, OLD's value at every other moment (trigger games_stamp_last_commit): the bot heartbeat's abandon guard. updated_at cannot serve it - the bot lease RPCs UPDATE games, so update_games_updated_at refreshes updated_at on the heartbeat's own drive and the guard never fires.
);

-- The same words as the line comments above, but stored in the catalog, which is
-- where the migrations put them (20260917140000, 20260918220000) and therefore
-- where a reader of the hosted database finds them. A line comment in this file
-- reaches nothing but this file.
COMMENT ON COLUMN games.state IS
  'The kernel''s durable board (c/src/table.h, v02): every hand and the deck order, a lobby included. SENSITIVE: service_role only.';
COMMENT ON COLUMN games.roster IS
  'The kernel''s durable roster (c/src/roster.h, format 1, 1227 bytes): each seat''s id, name and bot brain, and the table title. SENSITIVE: service_role only.';
COMMENT ON COLUMN games.needs_bots IS
  'PLAYING and a bot seat is still IN (the kernel''s table_needs_bots): the bot heartbeat''s scan predicate.';
COMMENT ON COLUMN games.logs_packed IS
  'The session log: kernel log records with u48 timestamps, DRAW identities pre-masked, appended by commit_table. Empty in a lobby and once the replay snapshot is stored.';
COMMENT ON COLUMN games.last_commit_at IS
  'now() at the moment `version` last changed, and OLD''s value at every other moment (trigger games_stamp_last_commit). The bot heartbeat''s abandon guard: a lease acquire/release, or any other write that is not a kernel commit, leaves it alone. updated_at cannot serve - the lease RPCs bump it through update_games_updated_at, which is what made the guard unfirable.';

-- Game decks table - SENSITIVE: Only edge functions can access
-- MEMBERSHIP: which humans are in which game. Named for the hands it used to
-- carry - the dealt state is games.state, and the hand/awaiting_attack columns
-- were write-only by the time they were dropped (migration 20260906120000).
-- Load-bearing even though it holds no cards: the realtime RLS policies below
-- EXISTS over it to answer "is this user in this game".
CREATE TABLE player_hands (
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  player_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  joined_at TIMESTAMP DEFAULT NOW(),
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW(),
  PRIMARY KEY (game_id, player_id) -- One hand per player per game
);
COMMENT ON TABLE player_hands IS
  'Membership: which humans are in which game. Named for the hands it used to carry; the dealt state is games.state. The realtime RLS policies EXISTS over this table, so it is load-bearing even though it holds no cards.';

-- Chat messages table
CREATE TABLE chat_messages (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  message TEXT NOT NULL,
  is_system BOOLEAN NOT NULL DEFAULT false,
  created_at TIMESTAMP DEFAULT NOW()
);

-- User ELO ratings table. Also carries the (immutable) username so the
-- publicly-readable rating rows can be rendered as a leaderboard without
-- touching auth.users — see migration 20260702090000_leaderboard_usernames.
CREATE TABLE user_elo_ratings (
  user_id UUID PRIMARY KEY REFERENCES auth.users(id) ON DELETE CASCADE,
  username TEXT,
  elo_rating INTEGER NOT NULL DEFAULT 1000,
  previous_elo INTEGER NOT NULL DEFAULT 1000,
  games_played INTEGER NOT NULL DEFAULT 0,
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

-- Bots table - AI players with strategies
CREATE TABLE bots (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  nickname TEXT NOT NULL,
  strategy_key TEXT NOT NULL,
  elo_rating INTEGER NOT NULL DEFAULT 1000,
  previous_elo INTEGER NOT NULL DEFAULT 1000,
  games_played INTEGER NOT NULL DEFAULT 0,
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

-- Bot hands table - SENSITIVE: Only edge functions can access (similar to player_hands)
-- MEMBERSHIP: which bots are in which game. See player_hands.
CREATE TABLE bot_hands (
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  bot_id UUID NOT NULL REFERENCES bots(id) ON DELETE CASCADE,
  joined_at TIMESTAMP DEFAULT NOW(),
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW(),
  PRIMARY KEY (game_id, bot_id) -- One hand per bot per game
);
COMMENT ON TABLE bot_hands IS
  'Membership: which bots are in which game. See player_hands.';

-- NOTE: game_locks and bot_locks are GONE. Concurrency is now handled by
-- games.version (optimistic CAS via the commit_table RPC) and the games.bot_lease_*
-- columns (auto-expiring bot-loop lease). See migration 20260616030000.

-- (auto_discard_locks removed — it backed the 60s all-good auto-discard, which
-- is disabled in actions/good.ts; see migration 20260702090000.)

-- (The game_logs table was dropped in migration 20260708120000. The session log
-- is stored as packed bytes in games.logs_packed — bot belief imports and the
-- replay snapshot both read it from there. Nothing writes per-record log rows
-- anymore.)

-- Game snapshots - one row per finished session: the complete game compressed
-- by functions/_shared/common/replay/, stored as raw binary. `moves` is the rANS
-- move integer (decodes to the full game); `extras` is the optional names +
-- timing blob. The share code is derived: base32(moves) + '-' + base32(extras)
-- — the moves-only code is just the first part. Replaces the session's
-- packed session log (games.logs_packed), which is cleared after the snapshot
-- is verified and stored.
-- player_ids doubles as the read ACL and records seat order. game_id is
-- SET NULL on delete so replays outlive lobby deletion.
CREATE TABLE game_snapshots (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  game_id TEXT REFERENCES games(id) ON DELETE SET NULL,
  player_ids UUID[] NOT NULL DEFAULT '{}', -- player ids in seat order
  moves BYTEA NOT NULL,
  extras BYTEA,
  created_at TIMESTAMP DEFAULT NOW()
);

-- Player views - a server-written, client-read PERSONALIZED VIEW CACHE
-- (docs/PLAYER_VIEWS.md). One row per (game, human participant): `view` is that
-- player's ALREADY-MASKED packed single-game envelope (the same bytes the
-- get_game / get_my_games edge functions emit, decodable by the shared
-- decodePackedGame), so RLS `player_id = auth.uid()` is SUFFICIENT — the row is
-- pre-masked, nothing to hide on read. This lets the client load its dashboard
-- list as a plain indexed SELECT (no edge round-trip) and get live pushes over
-- Realtime. Written ONLY by the service role, inside commit_table / create_table's
-- version-fenced transaction, so the cache can never be torn from games.state.
-- The `view` blob is safe to expose (masked for its owner); it must NEVER carry
-- the raw games.state.
CREATE TABLE player_views (
  game_id    TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  player_id  UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  view       BYTEA NOT NULL,           -- MASKED packed view envelope (PostgREST and realtime send it as '\x'-hex text)
  version    BIGINT NOT NULL,          -- mirrors games.version (optimistic token); client drops stale/reordered
  status     TEXT NOT NULL,            -- denormalized game_status for cheap list filtering/rendering
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (game_id, player_id)
);

-- Spectator views — the SHARED, fully-masked (seat -1) view of a game, readable
-- by ANY authenticated user (docs/PLAYER_VIEWS.md). This replaces get_game's
-- spectate path: a non-participant has no player_views row (that table is keyed
-- by auth.uid()), so spectators read their initial snapshot here and get live
-- updates over the RLS-guarded game-<id> broadcast. `view` is fully masked
-- (every hand a card-back, deck order hidden), so exposing it to all
-- authenticated users is safe — it must NEVER carry the raw games.state. One row
-- per game, written by the service role in commit_table / create_table's version
-- fence, alongside the per-player rows.
CREATE TABLE spectator_views (
  game_id    TEXT PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE,
  view       BYTEA NOT NULL,           -- fully-masked packed spectator envelope ('\x'-hex text to a client)
  version    BIGINT NOT NULL,          -- mirrors games.version
  status     TEXT NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- =============================================================================
-- INDEXES: Create indexes for better performance
-- =============================================================================

CREATE INDEX idx_games_status ON games(status);
CREATE INDEX idx_player_hands_game_id ON player_hands(game_id);
CREATE INDEX idx_player_hands_player_id ON player_hands(player_id);
CREATE INDEX idx_chat_messages_game_id ON chat_messages(game_id);
CREATE INDEX idx_chat_messages_user_id ON chat_messages(user_id);
CREATE INDEX idx_chat_messages_created_at ON chat_messages(created_at);
CREATE INDEX idx_games_updated_at ON games(updated_at);
-- bot-heartbeat SCAN (every 10s): needs_bots + updated_at window
CREATE INDEX idx_games_bot_scan ON games(updated_at) WHERE needs_bots;
-- and the cron tick's gate (migration 20260918230000): leading on last_commit_at
-- so the EXISTS stops at the first live row instead of walking every stalled one
CREATE INDEX idx_games_bot_gate ON games(last_commit_at) WHERE needs_bots;
CREATE INDEX idx_user_elo_ratings_user_id ON user_elo_ratings(user_id);
CREATE INDEX idx_user_elo_ratings_elo_rating ON user_elo_ratings(elo_rating);
CREATE INDEX idx_bots_strategy_key ON bots(strategy_key);
CREATE INDEX idx_bots_elo_rating ON bots(elo_rating);
CREATE INDEX idx_bot_hands_game_id ON bot_hands(game_id);
CREATE INDEX idx_bot_hands_bot_id ON bot_hands(bot_id);
CREATE INDEX idx_game_snapshots_game_id ON game_snapshots(game_id);
CREATE INDEX idx_game_snapshots_created_at ON game_snapshots(created_at);
CREATE INDEX idx_game_snapshots_player_ids ON game_snapshots USING GIN (player_ids);
-- dashboard list read: the caller's rows, newest game first (see get_my_games /
-- the client's direct player_views SELECT).
CREATE INDEX idx_player_views_player ON player_views(player_id, updated_at DESC);

-- =============================================================================
-- ROW LEVEL SECURITY: Enable RLS on all tables
-- =============================================================================

ALTER TABLE games ENABLE ROW LEVEL SECURITY;
ALTER TABLE player_hands ENABLE ROW LEVEL SECURITY;
ALTER TABLE chat_messages ENABLE ROW LEVEL SECURITY;
ALTER TABLE user_elo_ratings ENABLE ROW LEVEL SECURITY;
ALTER TABLE bots ENABLE ROW LEVEL SECURITY;
ALTER TABLE bot_hands ENABLE ROW LEVEL SECURITY;
ALTER TABLE game_snapshots ENABLE ROW LEVEL SECURITY;
ALTER TABLE player_views ENABLE ROW LEVEL SECURITY;
ALTER TABLE spectator_views ENABLE ROW LEVEL SECURITY;

-- =============================================================================
-- RLS POLICIES: Security-first approach
-- =============================================================================

-- Games: no client privilege and no policy. The row holds the unmasked board
-- (every hand, the deck order) and the roster, and no client reads it: the web
-- and iOS read their masked envelopes from player_views / spectator_views. RLS
-- stays enabled with no policy, so a grant added by mistake still reads nothing.
-- Supabase grants ALL on every public table to anon and authenticated; REVOKE on
-- the table also revokes every column privilege. Mirrors migration
-- 20260918210000_table_contract.sql.
REVOKE ALL ON public.games FROM PUBLIC, anon, authenticated;

-- Player Hands: Players can ONLY see their own hands
CREATE POLICY "Player hands select policy" ON player_hands
  FOR SELECT USING (
    player_id = (select auth.uid()) OR 
    (select auth.role()) = 'service_role'
  );

CREATE POLICY "Player hands insert policy" ON player_hands
  FOR INSERT WITH CHECK (
    (select auth.role()) = 'service_role'
  );

CREATE POLICY "Player hands update policy" ON player_hands
  FOR UPDATE USING ((select auth.role()) = 'service_role');

CREATE POLICY "Player hands delete policy" ON player_hands
  FOR DELETE USING (
    (select auth.role()) = 'service_role'
  );

-- Chat messages: Players can view messages for games they're in
CREATE POLICY "Players can view chat messages for their games" ON chat_messages
  FOR SELECT USING (
    game_id IN (
      SELECT game_id FROM player_hands 
      WHERE player_id = (select auth.uid())
    )
  );

CREATE POLICY "Players can send chat messages to their games" ON chat_messages
  FOR INSERT WITH CHECK (
    game_id IN (
      SELECT game_id FROM player_hands 
      WHERE player_id = (select auth.uid())
    ) AND user_id = (select auth.uid())
  );

-- User ELO ratings: Read-only for authenticated users
CREATE POLICY "ELO ratings access policy" ON user_elo_ratings
  FOR SELECT USING (true);

CREATE POLICY "Only service role can modify ELO ratings" ON user_elo_ratings
  FOR INSERT WITH CHECK ((select auth.role()) = 'service_role');

CREATE POLICY "Only service role can update ELO ratings" ON user_elo_ratings
  FOR UPDATE USING ((select auth.role()) = 'service_role');

CREATE POLICY "Only service role can delete ELO ratings" ON user_elo_ratings
  FOR DELETE USING ((select auth.role()) = 'service_role');

-- Bots: Read-only for authenticated users (for lobby bot selection)
CREATE POLICY "Bots access policy" ON bots
  FOR SELECT USING (true);

CREATE POLICY "Only service role can modify bots" ON bots
  FOR INSERT WITH CHECK ((select auth.role()) = 'service_role');

CREATE POLICY "Only service role can update bots" ON bots
  FOR UPDATE USING ((select auth.role()) = 'service_role');

CREATE POLICY "Only service role can delete bots" ON bots
  FOR DELETE USING ((select auth.role()) = 'service_role');

-- Bot hands: ONLY service role can access (edge functions only)
CREATE POLICY "Only service role can access bot hands" ON bot_hands
  FOR ALL USING ((select auth.role()) = 'service_role');


-- Game snapshots: written by edge functions at game end; readable by the
-- players who were in that game (player_ids holds their auth uids in seat
-- order). The replay itself is shared by URL, so this only gates lookup.
CREATE POLICY "Service role can insert snapshots" ON game_snapshots
  FOR INSERT WITH CHECK ((select auth.role()) = 'service_role');

CREATE POLICY "Participants can read snapshots" ON game_snapshots
  FOR SELECT USING (
    (select auth.role()) = 'service_role'
    OR player_ids @> ARRAY[(select auth.uid())]
  );

-- Player views: read ONLY your own rows; the blob is already masked for its
-- owner, so this simple, auditable policy is the whole personalization boundary
-- (docs/PLAYER_VIEWS.md). NO client writes — there is deliberately no
-- INSERT/UPDATE/DELETE policy, so authenticated can only SELECT; the service
-- role (which bypasses RLS) is the sole writer, via commit_table / create_table.
CREATE POLICY "Players can read their own views" ON player_views
  FOR SELECT USING (player_id = (select auth.uid()));

-- RLS gates ROWS; the table privilege must also be granted. SELECT only — no
-- INSERT/UPDATE/DELETE grant to client roles.
GRANT SELECT ON public.player_views TO authenticated;

-- Spectator views: the row is FULLY masked (seat -1), so ANY authenticated user
-- may read it — this is exactly the spectate case (a non-participant viewing a
-- game). Mirrors the game-<id> broadcast policy (authenticated-only). No client
-- writes (service role only).
CREATE POLICY "Authenticated can read spectator views" ON spectator_views
  FOR SELECT USING ((select auth.role()) = 'authenticated');
GRANT SELECT ON public.spectator_views TO authenticated;

-- Table writes: service_role only. Supabase grants ALL on every public table to
-- anon and authenticated, so without this RLS is the only barrier, and a policy
-- that is missing or wrong (like the old games INSERT policy) is a hole. The one
-- write a client makes on purpose is sending chat, gated by the chat_messages
-- INSERT policy above, so it is granted back. SELECT is untouched. Must run after
-- every CREATE TABLE. Mirrors migration 20260917000000 and is asserted under the
-- platform defaults by e2e/db_platform_grants.test.ts.
DO $$
DECLARE
  rel regclass;
BEGIN
  FOR rel IN
    SELECT c.oid::regclass
    FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'public'
      AND c.relkind IN ('r', 'p', 'v', 'm', 'f')
  LOOP
    EXECUTE format(
      'REVOKE INSERT, UPDATE, DELETE, TRUNCATE, REFERENCES, TRIGGER ON %s FROM PUBLIC, anon, authenticated;',
      rel);
  END LOOP;
END $$;

GRANT INSERT ON public.chat_messages TO authenticated;

-- Realtime: publish player_views so the client can subscribe to its own rows
-- (RLS-enforced) and receive live view pushes on every commit. Guarded: the
-- supabase_realtime publication exists on the Supabase platform but not in the
-- bare-Postgres e2e harness (e2e/schema.sql), where this is a harmless no-op.
DO $$ BEGIN
  ALTER PUBLICATION supabase_realtime ADD TABLE public.player_views;
EXCEPTION
  WHEN undefined_object THEN NULL; -- no such publication (e2e harness): skip
  WHEN duplicate_object THEN NULL; -- already in the publication: idempotent
END $$;

-- =============================================================================
-- FUNCTIONS: Helper functions and triggers
-- =============================================================================

-- Functions for automatic updated_at timestamps
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER 
SET search_path = ''
AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- games.last_commit_at: now() when `version` changed, OLD's value otherwise.
-- Written on EVERY insert and update, so a writer cannot supply its own value
-- and cannot refresh it without committing. `version` moves in commit_table and
-- nowhere else, and that means a move happened - which is what
-- the bot heartbeat's abandon guard needs and what updated_at could not give
-- it, the bot lease being an UPDATE like any other. See migration
-- 20260918230000_heartbeat_liveness_and_gate.sql.
CREATE OR REPLACE FUNCTION games_stamp_last_commit()
RETURNS TRIGGER
LANGUAGE plpgsql
SET search_path = ''
AS $$
BEGIN
  IF TG_OP = 'INSERT' OR NEW.version IS DISTINCT FROM OLD.version THEN
    NEW.last_commit_at := now();
  ELSE
    NEW.last_commit_at := OLD.last_commit_at;
  END IF;
  RETURN NEW;
END;
$$;

-- Function to broadcast chat message changes
CREATE OR REPLACE FUNCTION public.chat_messages_changes()
RETURNS TRIGGER
SECURITY DEFINER
SET search_path = ''
LANGUAGE plpgsql
AS $$
BEGIN
  -- Broadcast to game-specific topic for chat messages
  PERFORM realtime.broadcast_changes(
    'chat:' || COALESCE(NEW.game_id, OLD.game_id)::text, 
    TG_OP,
    TG_OP,
    TG_TABLE_NAME,
    TG_TABLE_SCHEMA,
    NEW,
    OLD
  );
  RETURN COALESCE(NEW, OLD);
END;
$$;

-- Function to create default ELO rating for new users
CREATE OR REPLACE FUNCTION public.create_default_elo_rating()
RETURNS TRIGGER
SECURITY DEFINER
SET search_path = ''
LANGUAGE plpgsql
AS $$
BEGIN
  INSERT INTO public.user_elo_ratings (user_id, elo_rating, games_played, username)
  VALUES (NEW.id, 1000, 0, NEW.raw_user_meta_data->>'username')
  ON CONFLICT (user_id) DO UPDATE SET username = EXCLUDED.username;
  RETURN NEW;
END;
$$;

-- =============================================================================
-- TRIGGERS: Set up automatic triggers
-- =============================================================================

CREATE TRIGGER update_games_updated_at
  BEFORE UPDATE ON games
  FOR EACH ROW
  EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER games_stamp_last_commit
  BEFORE INSERT OR UPDATE ON games
  FOR EACH ROW
  EXECUTE FUNCTION games_stamp_last_commit();


CREATE TRIGGER update_player_hands_updated_at 
  BEFORE UPDATE ON player_hands
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_user_elo_ratings_updated_at 
  BEFORE UPDATE ON user_elo_ratings
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_bots_updated_at 
  BEFORE UPDATE ON bots
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_bot_hands_updated_at 
  BEFORE UPDATE ON bot_hands
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

-- Trigger for chat message changes
CREATE TRIGGER handle_chat_messages_changes
  AFTER INSERT OR UPDATE OR DELETE
  ON public.chat_messages
  FOR EACH ROW
  EXECUTE FUNCTION chat_messages_changes();

-- Trigger to create default ELO rating for new users. Also fires on metadata
-- UPDATE: the app has no rename flow, but GoTrue's updateUser (and the admin
-- dashboard) can change raw_user_meta_data, and the denormalized
-- user_elo_ratings.username copy must follow.
DROP TRIGGER IF EXISTS handle_new_user_elo_rating ON auth.users;
CREATE TRIGGER handle_new_user_elo_rating
  AFTER INSERT OR UPDATE OF raw_user_meta_data
  ON auth.users
  FOR EACH ROW
  EXECUTE FUNCTION create_default_elo_rating();

-- Reserve the bot-name prefix. Replay codes encode only the player NAME (not the
-- is_ai flag), so bot-vs-human must be recoverable from the name alone. Bots are
-- named with a leading '%'; humans may not use it anywhere in their username. A
-- single-byte ASCII prefix keeps the game_snapshots.extras blob tiny. This trigger
-- is the AUTHORITATIVE guard (the client-side check in AuthContext is only for
-- fast UX and is bypassable). NOTE: position() is a LITERAL substring search, so
-- '%' here is just the character, not a LIKE wildcard.
CREATE OR REPLACE FUNCTION public.enforce_username_not_bot()
RETURNS TRIGGER
SECURITY DEFINER
SET search_path = ''
LANGUAGE plpgsql
AS $$
BEGIN
  IF position('%' in coalesce(NEW.raw_user_meta_data->>'username', '')) > 0 THEN
    RAISE EXCEPTION 'username may not contain the reserved bot prefix (%%)'
      USING ERRCODE = 'check_violation';
  END IF;
  RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS enforce_username_not_bot ON auth.users;
CREATE TRIGGER enforce_username_not_bot
  BEFORE INSERT OR UPDATE ON auth.users
  FOR EACH ROW
  EXECUTE FUNCTION public.enforce_username_not_bot();

-- =============================================================================
-- CONCURRENCY RPCs (replace game_locks + bot_locks). See migration
-- 20260616030000_cas_concurrency.sql for the rationale. Kept identical here.
-- =============================================================================

-- The kernel writers (docs/C_GAME_SHAPE_MIGRATION.md 3.3).
-- Every product comes from one table_commit_products call: the state and roster
-- blobs, the status as GAME_STATUS_* (0 waiting, 1 playing, 2 game_over, the
-- enum's order), needs_bots, the log records, the views. CAS on version, log
-- append or reset, round_epoch, membership, view cache; a lobby commit's human
-- list also prunes the humans who left, in the same transaction as the roster
-- that no longer seats them. The blobs arrive base64 (a third fewer PostgREST
-- body bytes than hex, measured at the same latency) and are stored as bytes;
-- the views are parallel arrays; a NULL roster keeps the stored one; the result
-- is the OUT columns. Mirrors migration 20260918220000_table_bytea.sql.
CREATE OR REPLACE FUNCTION commit_table(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_state            TEXT,                  -- base64: the kernel's state blob
  p_status           SMALLINT,              -- GAME_STATUS_*: 0 waiting, 1 playing, 2 game_over (the enum's order)
  p_needs_bots       BOOLEAN,
  p_roster           TEXT    DEFAULT NULL,  -- base64: the durable roster; NULL keeps the stored one (the operation left it alone)
  p_seats            UUID[]  DEFAULT NULL,  -- human member ids; NULL leaves player_hands untouched
  p_bot_seats        UUID[]  DEFAULT NULL,  -- bot member ids; NULL leaves bot_hands untouched
  p_logs_packed      TEXT    DEFAULT NULL,  -- base64: this operation's log records, appended under the version fence
  p_logs_reset       BOOLEAN DEFAULT FALSE, -- the operation dealt: replace the session log instead of appending
  p_game_seed        TEXT    DEFAULT NULL,  -- deal seed (64 hex characters); NULL keeps the stored one
  p_view_players     UUID[]  DEFAULT NULL,  -- the human seats' ids, parallel to p_views; NULL leaves player_views untouched
  p_views            TEXT[]  DEFAULT NULL,  -- base64: each seat's envelope, parallel to p_view_players
  p_spectator        TEXT    DEFAULT NULL,  -- base64: the spectator envelope; NULL leaves spectator_views untouched
  p_closed_round     BOOLEAN DEFAULT FALSE, -- the operation closed a round: stamp round_epoch with the new version
  OUT committed       BOOLEAN,              -- FALSE: the version fence refused (another writer committed first)
  OUT new_version     BIGINT,
  OUT new_round_epoch BIGINT
)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_status game_status := (enum_range(NULL::game_status))[p_status + 1];
BEGIN
  IF p_state IS NULL OR p_needs_bots IS NULL THEN
    RAISE EXCEPTION 'commit_table: state and needs_bots are required' USING ERRCODE = 'null_value_not_allowed';
  END IF;
  IF v_status IS NULL THEN
    RAISE EXCEPTION 'commit_table: % is not a GAME_STATUS', p_status USING ERRCODE = 'invalid_parameter_value';
  END IF;
  IF (p_view_players IS NULL) <> (p_views IS NULL) OR cardinality(p_view_players) <> cardinality(p_views) THEN
    RAISE EXCEPTION 'commit_table: p_view_players and p_views go together, one envelope per player' USING ERRCODE = 'invalid_parameter_value';
  END IF;

  UPDATE games SET
    status = v_status, state = decode(p_state, 'base64'), roster = COALESCE(decode(p_roster, 'base64'), roster), needs_bots = p_needs_bots,
    game_seed = CASE WHEN v_status = 'waiting' THEN NULL ELSE COALESCE(p_game_seed, game_seed) END,
    logs_packed = CASE
      WHEN p_logs_reset THEN COALESCE(decode(p_logs_packed, 'base64'), ''::bytea)
      WHEN v_status = 'waiting' THEN ''::bytea
      ELSE COALESCE(logs_packed, ''::bytea) || COALESCE(decode(p_logs_packed, 'base64'), ''::bytea)
    END,
    round_epoch = CASE
      WHEN p_logs_reset OR v_status = 'waiting' THEN 0
      WHEN p_closed_round THEN version + 1
      ELSE round_epoch
    END,
    updated_at = now(), version = version + 1
  WHERE id = p_game_id AND version = p_expected_version
  RETURNING version, round_epoch INTO new_version, new_round_epoch;

  committed := FOUND;
  IF NOT committed THEN
    RETURN;
  END IF;

  IF p_seats IS NOT NULL THEN
    INSERT INTO player_hands (game_id, player_id)
    SELECT p_game_id, s FROM unnest(p_seats) AS s
    ON CONFLICT (game_id, player_id) DO UPDATE SET updated_at = now();
    DELETE FROM player_hands
    WHERE game_id = p_game_id AND player_id <> ALL (p_seats);
  END IF;

  IF p_bot_seats IS NOT NULL THEN
    INSERT INTO bot_hands (game_id, bot_id)
    SELECT p_game_id, b FROM unnest(p_bot_seats) AS b
    ON CONFLICT (game_id, bot_id) DO UPDATE SET updated_at = now();
    DELETE FROM bot_hands
    WHERE game_id = p_game_id AND bot_id <> ALL (p_bot_seats);
  END IF;

  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, v.player_id, decode(v.view, 'base64'), new_version, v_status::text, now()
    FROM unnest(p_view_players, p_views) AS v(player_id, view)
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
    DELETE FROM player_views
    WHERE game_id = p_game_id AND player_id <> ALL (p_view_players);
  END IF;

  IF p_spectator IS NOT NULL THEN
    INSERT INTO spectator_views (game_id, view, version, status, updated_at)
    VALUES (p_game_id, decode(p_spectator, 'base64'), new_version, v_status::text, now())
    ON CONFLICT (game_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;
END;
$$;

-- A new lobby from table_create's products. A unique violation on the games row
-- means the id is taken: the create function draws another.
CREATE OR REPLACE FUNCTION create_table(
  p_game_id   TEXT,
  p_player_id UUID,
  p_state     TEXT,               -- base64: the lobby's state blob
  p_roster    TEXT,               -- base64: the durable roster, the creator seated
  p_view      TEXT DEFAULT NULL,  -- base64: the creator's envelope; version 0
  p_spectator TEXT DEFAULT NULL   -- base64: the spectator envelope; version 0
) RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  IF p_state IS NULL OR p_roster IS NULL THEN
    RAISE EXCEPTION 'create_table: state and roster are required' USING ERRCODE = 'null_value_not_allowed';
  END IF;

  INSERT INTO games (id, status, state, roster, needs_bots)
    VALUES (p_game_id, 'waiting', decode(p_state, 'base64'), decode(p_roster, 'base64'), FALSE);

  INSERT INTO player_hands (game_id, player_id)
    VALUES (p_game_id, p_player_id);

  IF p_view IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    VALUES (p_game_id, p_player_id, decode(p_view, 'base64'), 0, 'waiting', now())
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;

  IF p_spectator IS NOT NULL THEN
    INSERT INTO spectator_views (game_id, view, version, status, updated_at)
    VALUES (p_game_id, decode(p_spectator, 'base64'), 0, 'waiting', now())
    ON CONFLICT (game_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;
END;
$$;

-- Account deletion (migrations 20260714120000, 20260918210000). The seat names
-- in games.roster and in every cached view are redacted by the delete-account
-- edge function before it calls this (table_redact per seated table, committed
-- through commit_table; docs/C_GAME_SHAPE_MIGRATION.md Q8). Owned rows cascade
-- from auth.users when the function then deletes the user; this clears the
-- denormalized leaderboard username in case that foreign key is ever SET NULL.
CREATE OR REPLACE FUNCTION public.delete_account(p_user_id UUID)
RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  UPDATE public.user_elo_ratings
  SET username = NULL
  WHERE user_id = p_user_id;
END;
$$;

-- Bot-loop lease: atomic claim (NULL if another loop holds a live lease).
CREATE OR REPLACE FUNCTION try_acquire_bot_lease(p_game_id TEXT, p_ttl_ms INT)
RETURNS UUID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_token UUID;
BEGIN
  UPDATE games SET
    bot_lease_token = gen_random_uuid(),
    bot_lease_until = now() + make_interval(secs => p_ttl_ms / 1000.0)
  WHERE id = p_game_id
    AND (bot_lease_until IS NULL OR bot_lease_until < now())
  RETURNING bot_lease_token INTO v_token;
  RETURN v_token;
END;
$$;

CREATE OR REPLACE FUNCTION release_bot_lease(p_game_id TEXT, p_token UUID)
RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  UPDATE games SET bot_lease_until = now() - interval '1 second'
  WHERE id = p_game_id AND bot_lease_token = p_token;
END;
$$;

-- Extend our lease each cycle so a long loop keeps a SHORT TTL (fast recovery).
CREATE OR REPLACE FUNCTION renew_bot_lease(p_game_id TEXT, p_token UUID, p_ttl_ms INT)
RETURNS BOOLEAN
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_rows INT;
BEGIN
  UPDATE games SET bot_lease_until = now() + make_interval(secs => p_ttl_ms / 1000.0)
  WHERE id = p_game_id AND bot_lease_token = p_token;
  GET DIAGNOSTICS v_rows = ROW_COUNT;
  RETURN v_rows > 0;
END;
$$;

-- SECURITY: every SECURITY DEFINER function in public is service_role only.
-- They bypass RLS by design (the edge functions call them with the service-role
-- key). Postgres grants EXECUTE to PUBLIC on every new function and Supabase's
-- default privileges add anon and authenticated, so without this anon (the role
-- behind the public anon key) could call them through PostgREST and rewrite any
-- game. No client calls any RPC. Revoking from PUBLIC alone is not enough, hence
-- all three. Not a list of names: a migration that DROPs and re-CREATEs one gets
-- fresh default grants, which is how commit_game reopened on the hosted project
-- (20260906120000, relocked by 20260917000000). Trigger functions are skipped;
-- PostgREST does not expose them. Must run after every definer function is
-- created; a function a client is meant to call must be granted explicitly
-- after this. See migrations 20260807120000 and
-- 20260917000000, e2e/db_grants.test.ts and e2e/db_platform_grants.test.ts.
DO $$
DECLARE
  fn regprocedure;
BEGIN
  FOR fn IN
    SELECT p.oid::regprocedure
    FROM pg_proc p
    JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'public'
      AND p.prosecdef
      AND p.prorettype <> 'trigger'::regtype
  LOOP
    EXECUTE format('REVOKE ALL ON FUNCTION %s FROM PUBLIC, anon, authenticated;', fn);
    -- REVOKE FROM PUBLIC also strips service_role's implicit grant, so re-grant.
    EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO service_role;', fn);
  END LOOP;
END $$;

-- =============================================================================
-- REALTIME AUTHORIZATION POLICIES
-- Enable Supabase Realtime with proper security
-- =============================================================================

-- Enable RLS on the realtime messages table. Guarded: on the Supabase images
-- supabase_realtime_admin owns realtime.messages with RLS already on, and
-- `postgres` (which runs this file on `supabase start`) may create policies on
-- it but not ALTER it ("must be owner of table messages"). The e2e shim's table
-- (e2e/schema.sql) starts with RLS off, and its superuser turns it on here.
DO $$ BEGIN
  IF EXISTS (SELECT 1 FROM pg_class WHERE oid = to_regclass('realtime.messages') AND NOT relrowsecurity) THEN
    ALTER TABLE realtime.messages ENABLE ROW LEVEL SECURITY;
  END IF;
END $$;

-- Drop existing policies if they exist
DROP POLICY IF EXISTS "authenticated can receive game broadcasts" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can receive private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can send private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can receive game-user messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can receive chat broadcasts" ON "realtime"."messages";
DROP POLICY IF EXISTS "service role can send game broadcasts" ON "realtime"."messages";
DROP POLICY IF EXISTS "service role can send private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "service role can send game-user messages" ON "realtime"."messages";

-- Policy for public game channels (topic: game-{game_id})
-- Anyone can read game broadcasts since it's public information
CREATE POLICY "authenticated can receive game broadcasts"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'game-%'
  AND realtime.messages.extension IN ('broadcast')
);


-- (No user-{email} channel policies: nothing uses that topic family, and its
-- receive policy matched on the email's local part. Dropped on hosted by
-- migrations/20260917130000_drop_user_realtime_policies.sql.)

-- Policy for private game-user channels (topic: gu-{game_id}-{user_id})
-- A user reads only their own seat's stream, for a game they are in. The topic is
-- rebuilt from the membership row and compared whole, never parsed: a user id is a
-- hyphenated UUID, so splitting the topic on '-' cannot recover it (see
-- migrations/20260917120000_realtime_channel_exact_topics.sql).
CREATE POLICY "authenticated can receive game-user messages"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'gu-%' AND
  EXISTS (
    SELECT 1
    FROM public.player_hands ph
    WHERE ph.player_id = (SELECT auth.uid())
      AND (SELECT realtime.topic()) = 'gu-' || ph.game_id || '-' || (SELECT auth.uid())::text
  ) AND
  realtime.messages.extension IN ('broadcast')
);

-- Policy for chat broadcasts (topic: chat:{game_id})
-- Users can receive chat broadcasts for games they're participating in, on the
-- exact topic the chat_messages_changes trigger broadcasts to.
CREATE POLICY "authenticated can receive chat broadcasts"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'chat:%' AND
  EXISTS (
    SELECT 1
    FROM public.player_hands ph
    WHERE ph.player_id = (SELECT auth.uid())
      AND (SELECT realtime.topic()) = 'chat:' || ph.game_id
  ) AND
  realtime.messages.extension IN ('broadcast')
);

-- ===============================
-- SERVICE ROLE POLICIES FOR SERVER-SIDE FUNCTIONS
-- Allow Supabase functions to send broadcasts
-- =============================================================================

-- Service role can send game broadcasts (for server-initiated game updates)
CREATE POLICY "service role can send game broadcasts"
ON "realtime"."messages" 
FOR INSERT
TO service_role
WITH CHECK (
  (SELECT realtime.topic()) LIKE 'game-%'
  AND realtime.messages.extension IN ('broadcast')
);

-- Service role can send game-user messages (for server-initiated personalized updates)
CREATE POLICY "service role can send game-user messages"
ON "realtime"."messages"
FOR INSERT
TO service_role
WITH CHECK (
  (SELECT realtime.topic()) LIKE 'gu-%'
  AND realtime.messages.extension IN ('broadcast')
);



-- =============================================================================
-- SCHEDULED JOBS: the bot heartbeat, and the VACUUM that keeps pg_net's log small
-- =============================================================================
-- These two pg_cron entries are RUNNING INFRASTRUCTURE, not schema. They used to
-- live only in the migration history (20260616040000_bot_heartbeat_cron,
-- 20260712120000_heartbeat_prune_cron_history,
-- 20260918200000_pg_net_response_log_retention,
-- 20260918230000_heartbeat_liveness_and_gate); when that history was retired this
-- file became their only definition, so a database built from seed.sql alone has
-- a bot loop and a bounded response log. Without them a fresh database has no bot
-- loop at all - bots never move in a game with nobody watching it - and pg_net's
-- response log grows without bound until it eats the disk.
--
-- WHY THIS SECTION IS GUARDED
--
-- pg_cron and pg_net are platform extensions. The hosted project and a local
-- `supabase start` both have them; the bare Postgres the e2e harness runs on has
-- neither, and gets the shapes instead (e2e/fixtures/platform_extensions.sql,
-- applied by the harness before this file so the jobs below are really
-- scheduled and really asserted). A Postgres with neither gets the schema and no
-- jobs rather than an error, because the gameplay schema above is the part every
-- database needs and the scheduler is the part only a server needs.
--
-- IDEMPOTENT. CREATE EXTENSION IF NOT EXISTS, and cron.schedule() upserts on
-- (jobname, username), so re-running this file replaces a job rather than adding
-- a second one.

DO $$
BEGIN
  IF to_regnamespace('cron') IS NULL
     AND EXISTS (SELECT 1 FROM pg_available_extensions WHERE name = 'pg_cron') THEN
    EXECUTE 'CREATE EXTENSION IF NOT EXISTS pg_cron';
  END IF;
  IF to_regnamespace('net') IS NULL
     AND EXISTS (SELECT 1 FROM pg_available_extensions WHERE name = 'pg_net') THEN
    EXECUTE 'CREATE EXTENSION IF NOT EXISTS pg_net';
  END IF;
END $$;

DO $$
BEGIN
  IF to_regprocedure('cron.schedule(text,text,text)') IS NULL THEN
    RAISE NOTICE 'pg_cron is not installed: the bot heartbeat and the pg_net response-log VACUUM are NOT scheduled on this database. That is expected on a plain Postgres and wrong on a server.';
    RETURN;
  END IF;

  -- ---------------------------------------------------------------------------
  -- 1. The bot heartbeat
  -- ---------------------------------------------------------------------------
  -- A DUMB TRIGGER: every 10s it POSTs the bot-heartbeat edge function's SCAN
  -- endpoint (empty body). ALL "which games need driving" logic lives in the
  -- function (TypeScript), not here.
  --
  -- PREREQUISITES on a server:
  --   1. Deploy the `bot-heartbeat` edge function.
  --   2. Store the service-role key in Vault so it isn't written in plaintext:
  --        select vault.create_secret('<YOUR_SERVICE_ROLE_KEY>', 'service_role_key');
  --      (Settings > API has the service_role key. Run create_secret once.)
  --
  -- '10 seconds' is pg_cron's sub-minute interval syntax (pg_cron >= 1.5;
  -- Supabase ships a newer one). An instance that rejects it falls back to
  -- '* * * * *', but that makes bots-only games lurch in 60s bursts.
  --
  -- Cost: pg_cron and pg_net are free. The only metered cost is edge
  -- invocations, which is what the WHERE EXISTS below is about.
  --
  -- THE PRUNE. cron.job_run_details appends a row on EVERY job run and never
  -- prunes itself. At ~8,600 runs a day it reached ~224k rows / 151 MB in 26
  -- days and, with pg_net's response log, blew past the 500 MB free-tier cap.
  -- The DELETE scans only the last ~2 days of rows (a few thousand) and removes
  -- anything older, so the table stays small forever. end_time IS NULL for an
  -- in-flight run and `NULL < ...` is NULL, so a running job is never deleted.
  -- It lives inside the heartbeat job rather than in a cron entry of its own, at
  -- the operator's request.
  --
  -- THE GATE. Measured on hosted (wngpfwmwkltonwosqflx, read-only) on
  -- 2026-09-18: 8,622 runs in 24 h => 258,660 net.http_post calls / 30 days,
  -- every one an edge invocation, against a 500,000 free-tier month of which
  -- 277,366 was already spent. Of 90 games exactly ONE was playing - 24a407,
  -- created 2026-07-13 - with version and round_epoch frozen across two dumps
  -- 410 s apart while updated_at advanced the whole time and sat at exactly
  -- bot_lease_until + 1 second in both. That +1 second is release_bot_lease: it
  -- sets bot_lease_until = now() - 1s and update_games_updated_at stamps
  -- updated_at on the same UPDATE. So the heartbeat, driving a game abandoned 67
  -- days earlier, refreshed the very column the scan's one-hour ABANDON bound
  -- reads. The guard meant to stop this was the thing the loop kept resetting.
  --
  -- games.last_commit_at (declared on the games table above, written by the
  -- games_stamp_last_commit trigger) is the liveness clock the loop cannot wind:
  -- it moves only when `version` moves, and `version` moves in exactly one place
  -- in the whole schema, commit_table's `version = version + 1`, which is a
  -- kernel commit. The gate reads it.
  --
  -- The cadence does not change: 10 seconds while a game is live, because gating
  -- costs no latency and widening the interval buys the same saving by making
  -- bots slower for the people who ARE playing. The gate is deliberately WEAKER
  -- than the scan's own filter - it asks only for a row the kernel says needs
  -- bots whose last commit is inside the same one-hour window, and leaves the
  -- 10-second staleness test where it already lives, in the function. So every
  -- tick that would have dispatched still posts, and the ticks that now cost
  -- nothing are exactly the ticks that used to post and dispatch nothing.
  --
  -- What "something to drive" means is not a guess: games.needs_bots IS the
  -- kernel's verdict (c/src/table.h table_needs_bots - PLAYING, and a bot seat
  -- still IN), written by every commit, and it is already the scan's predicate.
  -- A WAITING lobby is never the heartbeat's business, and the lobby paths wake
  -- the bots inline through scheduleBotLoop on the commit that starts the game.
  --
  -- SHAPE NOTES, because pg_cron sends this as a SIMPLE QUERY on this instance
  -- (cron.use_background_workers is off), which makes the two statements one
  -- implicit transaction block:
  --   * Both statements are happy there.
  --   * `SELECT f() WHERE <false>` returns zero rows and never evaluates f().
  --     That is what makes the tick free: no request is queued, no response row
  --     is written, no function is invoked.
  --   * now() is transaction time, so the gate and the DELETE see one clock.
  -- The interval below is the scan's ABANDON_MS (functions/bot-heartbeat/index.ts)
  -- and the two are asserted equal in e2e/heartbeat_gate.test.ts - a gate
  -- NARROWER than the scan would strand a game the scan was willing to drive.
  --
  -- The URL is the hosted project's, as it has been since this job was first
  -- scheduled. A local stack therefore posts at hosted's bot-heartbeat with a
  -- service-role key its Vault does not have, which 401s and drives nothing; the
  -- local bot loop is woken inline by scheduleBotLoop instead.
  PERFORM cron.schedule(
    'bot-heartbeat',
    '10 seconds',
    $job$
  DELETE FROM cron.job_run_details WHERE end_time < now() - interval '2 days';
  SELECT net.http_post(
    url := 'https://wngpfwmwkltonwosqflx.supabase.co/functions/v1/bot-heartbeat',
    headers := jsonb_build_object(
      'Content-Type',  'application/json',
      'Authorization', 'Bearer ' || (SELECT decrypted_secret FROM vault.decrypted_secrets WHERE name = 'service_role_key'),
      'apikey',        (SELECT decrypted_secret FROM vault.decrypted_secrets WHERE name = 'service_role_key')
    ),
    body := '{}'::jsonb,
    timeout_milliseconds := 5000
  )
  WHERE EXISTS (
    SELECT 1 FROM games
    WHERE needs_bots
      AND last_commit_at > now() - interval '1 hour'
  );
  $job$
  );

  -- ---------------------------------------------------------------------------
  -- 2. The VACUUM that keeps pg_net's response log from eating the disk
  -- ---------------------------------------------------------------------------
  -- Measured on hosted on 2026-09-18: the database was 542.5 MB against the
  -- 500 MB free-plan cap, and net._http_response was 511.1 MB of it - holding
  -- 2,155 live rows totalling 2.5 MB. The heap was 63,763 pages and every live
  -- row sat in the LAST 360 of them; pages 0..63,402, 495 MB, held nothing.
  -- autovacuum_count was 1, ever.
  --
  -- Neither usual suspect is guilty. pg_net's TTL is the 6-hour default and it
  -- works - the live window is exactly six hours wide - and one POST every 10 s
  -- is only 8,640 responses a day. What is wrong is a loop that sustains itself:
  --
  --   1. pg_net's reaper DELETEs rows past the TTL. Its own sequential scan
  --      prunes those dead tuples off the page as it goes, which frees space
  --      WITHIN the page but does not touch the free space map - only VACUUM
  --      writes that.
  --   2. Because the pruning keeps n_dead_tup near zero (4, against 431,140
  --      lifetime deletes), the table never crosses the autovacuum threshold of
  --      50 + 0.2 x 2,141 ~ 478 dead tuples. Autovacuum ran on it ONCE.
  --   3. With no free space map entries, every INSERT extends the relation
  --      instead of reusing a page. Go to 1.
  --
  -- 8,640 rows a day at 6 rows to a page is ~1,440 new pages, ~11.8 MB a day.
  --
  -- A plain VACUUM writes the free space map, so the next insert reuses a reaped
  -- page rather than extending the file, and the heap settles at the live window
  -- (~2,160 rows, ~360 pages, ~3 MB) plus at most one interval's worth of new
  -- pages - every 15 minutes that is 90 rows, ~15 pages, ~120 kB. Under 5 MB
  -- with the index, against 511 MB. e2e/pg_net_log_retention.test.ts runs two
  -- days of the production cycle twice, once with this command and once without,
  -- and holds the two apart.
  --
  -- ONE BARE STATEMENT in the job, deliberately: cron.use_background_workers is
  -- off on this instance, so pg_cron sends the command to a libpq backend as a
  -- simple query. A multi-statement command would arrive as an implicit
  -- transaction block and Postgres refuses VACUUM inside one.
  --
  -- WHAT THIS DOES NOT DO, and why:
  --
  --   cron.job_run_details is left to the heartbeat's own DELETE above. It is
  --   14.5 MB of 16,832 LIVE rows - no dead space to reclaim, already bounded,
  --   autovacuumed 159 times. It is 2.6% of the database and it is not sick.
  --
  --   ALTER TABLE net._http_response SET (autovacuum_vacuum_insert_threshold)
  --   would be tidier - insert-driven autovacuum, PG 13+, and this instance is
  --   17.4 - but the table is owned by supabase_admin and we are postgres, which
  --   is not a member of it, so ALTER TABLE is refused. postgres does hold
  --   MAINTAIN on it, which is what VACUUM needs.
  --
  --   The heartbeat's frequency is untouched. The pg_net row rate IS the cron
  --   rate and nothing else - the per-game drive dispatches are fetch() calls
  --   from inside the edge function, not pg_net - so changing it is a gameplay
  --   decision, not a storage one.
  --
  -- The 511 MB itself was handed back by a one-time `TRUNCATE net._http_response`
  -- (a DELETE would have left the file exactly as long as it found it) in the
  -- migration this section replaces. That reclaim has happened; a database built
  -- from this file has nothing to reclaim, so only the part that keeps it fixed
  -- is here.
  PERFORM cron.schedule(
    'pg-net-response-vacuum',
    '*/15 * * * *',
    'VACUUM net._http_response'
  );
END $$;

-- To check on them later:
--   select jobname, schedule, active from cron.job;
--   select status, return_message, start_time from cron.job_run_details
--     where jobid = (select jobid from cron.job where jobname = 'bot-heartbeat')
--     order by start_time desc limit 10;
--   select count(*) from net._http_response where created > now() - interval '1 hour';
--   select pg_size_pretty(pg_total_relation_size('net._http_response'));
--   select id, version, updated_at, last_commit_at from games where needs_bots;


-- =============================================================================
-- SEED DATA: Initial bots with different strategies
-- =============================================================================

INSERT INTO bots (nickname, strategy_key) VALUES
-- Handwritten strategy bots (rule-based)
--
-- SEVEN OF EVERY FAMILY, which is what makes a full table of one bot possible:
-- eight seats is a human plus seven opponents, so a rung with fewer than seven
-- rows cannot fill one. Every seeded family below is seven for that reason, and
-- e2e/validation/bot_city_names_validation.test.ts holds it.
--
-- The `0x00C0FFEE` row is gone (it was the second Handwritten). It predates the
-- city ladder and was the one seeded nickname that is not a rung's name at all,
-- so on a board of Miami / New York / Madrid it read as a bug rather than as a
-- joke. The nickname parser still passes a leading `0x...` through verbatim,
-- because old replay blobs carry it embedded at encode time.
('Handwritten 1', 'handwritten'),
('Handwritten 2', 'handwritten'),
('Handwritten 3', 'handwritten'),
('Handwritten 4', 'handwritten'),
('Handwritten 5', 'handwritten'),
('Handwritten 6', 'handwritten'),
('Handwritten 7', 'handwritten'),

-- Random strategy bots (chaotic)
('Random 1', 'random'),
('Random 2', 'random'),
('Random 3', 'random'),
('Random 4', 'random'),
('Random 5', 'random'),
('Random 6', 'random'),
('Random 7', 'random'),

-- NOTE: simple_heuristic is NOT seeded, though the kernel dispatches it fine.
-- The site renders a bot seat as its rung's city (docs/IOS_BOT_NAMING.md), the
-- ladder is seven cities, and this rung has none - a seeded row would put
-- "Simple Heuristic 2" on a board between Miami and Madrid. It stays an
-- offline-only rung in c/src/bot_roster.c (`offline` 1, `seeded` 0); migration
-- 20260922120000_city_ladder_bot_rows took its three rows off hosted.

-- NOTE: champion, ultimate_champion, hacker, espresso, semtex and semtex_max
-- are intentionally NOT seeded. Those strategies are not compiled into / not
-- dispatched by the production bots.wasm (see wasm_choose_move in
-- c/wasm/wasm_bots_api.c), so a bot carrying one of those keys silently
-- falls back to `random` — a bot that plays nothing like its name and pollutes
-- the Elo leaderboard. Only strategy keys the wasm actually dispatches are
-- seeded: random, handwritten (→handwritten_prod), firecracker, blackpowder,
-- cordite, octogen.
--
-- The seeded set must equal the `seeded` column of the C bot roster
-- (c/src/bot_roster.c) — that table is the canonical roster
-- (docs/C_CORE_CONSOLIDATION.md F1).
--
-- The `_max` tiers were retired (migration 20260715120000_drop_max_bot_tiers):
-- octogen_max was a plain alias of octogen (identical knobs), and cordite_max's
-- CD_BUDGET=max is a FLAT world budget that only exceeds the player-count-aware
-- `prod` schedule at 2-4 players and is about HALF of it at 6-8 — so "Max" was
-- the weaker bot in the larger games. One cordite, on the prod budget.

-- Robusta strategy bots - the ladder's Seoul, tier 5. The first Monte-Carlo
-- bot: public-info sampled worlds, no belief and no solver. It was offline-only
-- until 2026-09-22, which left the site jumping New York (tier 3) straight to
-- Madrid (tier 6) and one of the ladder's seven cities unreachable. The shipped
-- bots.wasm has always LINKED it for free - firecracker is robusta's MC, so the
-- code is there either way - which is what made seeding it a one-flag change.
('Robusta 1', 'robusta'),
('Robusta 2', 'robusta'),
('Robusta 3', 'robusta'),
('Robusta 4', 'robusta'),
('Robusta 5', 'robusta'),
('Robusta 6', 'robusta'),
('Robusta 7', 'robusta'),

-- Firecracker strategy bots — shipped ladder "Medium" rung (Durak Bot Ordnance
-- Chart). Public-info Monte Carlo: robusta's sampled-world MC with espresso as
-- the rollout policy. Honest (never reads real hidden hands).
('Firecracker 1', 'firecracker'),
('Firecracker 2', 'firecracker'),
('Firecracker 3', 'firecracker'),
('Firecracker 4', 'firecracker'),
('Firecracker 5', 'firecracker'),
('Firecracker 6', 'firecracker'),
('Firecracker 7', 'firecracker'),

-- Blackpowder strategy bots — shipped ladder "Hard" rung. The first
-- belief-constrained Monte Carlo (cordite's predecessor): card memory rebuilt
-- from the public log, void-constraint belief mixture, and an exact endgame
-- solver. Public info only.
('Blackpowder 1', 'blackpowder'),
('Blackpowder 2', 'blackpowder'),
('Blackpowder 3', 'blackpowder'),
('Blackpowder 4', 'blackpowder'),
('Blackpowder 5', 'blackpowder'),
('Blackpowder 6', 'blackpowder'),
('Blackpowder 7', 'blackpowder'),

-- Cordite strategy bots (belief-constrained Monte Carlo, no cheating —
-- beats every other bot at every player count 2-8; see c/CORDITE.md)
('Cordite 1', 'cordite'),
('Cordite 2', 'cordite'),
('Cordite 3', 'cordite'),
('Cordite 4', 'cordite'),
('Cordite 5', 'cordite'),
('Cordite 6', 'cordite'),
('Cordite 7', 'cordite'),

-- (semtex / semtex_max are not seeded — not dispatched by bots.wasm; see the
-- note above. Octogen is semtex's shipped successor and IS dispatched.)

-- Octogen (semtex + extended exact-solve window; provably never worse than
-- semtex, strictly better in deep heads-up endgames — see c/OCTOGEN.md)
('Octogen 1', 'octogen'),
('Octogen 2', 'octogen'),
('Octogen 3', 'octogen'),
('Octogen 4', 'octogen'),
('Octogen 5', 'octogen'),
('Octogen 6', 'octogen'),
('Octogen 7', 'octogen');

-- Bots carry the reserved '%' prefix so bot-vs-human is recoverable from the
-- name-only replay codec. Done as an UPDATE (rather than prefixing every literal
-- above) so the list stays readable; idempotent via the left() check. The hosted
-- database was given the same rename by migration 20260615120000.
UPDATE bots SET nickname = '%' || nickname WHERE left(nickname, 1) <> '%';


-- =============================================================================
-- SETUP COMPLETE!
-- Your database schema is now secure and ready for the game application.
-- Advisory locks are configured for game operation synchronization.
-- Bot system is initialized with sample strategies.
-- =============================================================================
