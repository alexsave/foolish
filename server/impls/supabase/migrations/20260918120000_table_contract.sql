-- CONTRACT: games is the kernel's two blobs and the scalars SQL filters on
-- (docs/C_GAME_SHAPE_MIGRATION.md 3.1, 3.3, 3.4, Phase 4c).
--
-- The third of three deploys:
--
--   4a expand   (20260917140000)  every row gained `roster` and a lobby `state`,
--                                 and a bridge trigger kept them in step with the
--                                 JSONB the pre-4b functions still wrote
--   4b switch   (edge functions)  every read and write is `state` + `roster`
--                                 through commit_table / create_table
--   4c contract (this file)       the JSONB game shape goes
--
-- DEPLOY ONLY AFTER THE 4b FUNCTIONS ARE LIVE AND VERIFIED. The pre-4b functions
-- call commit_game and create_game and read `players`; every one of their
-- requests fails once this runs. deploy.yml pushes migrations BEFORE it deploys
-- functions, so this file must not reach main in the same push as the 4b
-- functions. Step 0 refuses to run while no row is owned by the kernel writers,
-- which is what a database the 4b functions never wrote to looks like; the
-- refusal stops the deploy job before its functions step.
--
-- WHAT THIS DOES
--
-- 0. Refuses: no row owned by the kernel writers (4b not live), or any row
--    without both blobs (nothing a 4a-or-later writer can leave).
-- 1. The bridge goes: the games_legacy_bridge trigger and its function, and the
--    SQL restatement of the kernel's byte layouts (legacy_roster_hex,
--    legacy_lobby_state_hex, legacy_roster_field, legacy_utf8_cut). The kernel is
--    the one place that knows them again.
-- 2. The legacy writers go: commit_game and create_game, every overload.
-- 3. Clients lose games entirely: the "Anyone can view games" policy (and the
--    long-dropped create policy, if a database still has it), and every table
--    and column privilege of anon and authenticated. No client reads games: the
--    web reads player_views, spectator_views, game_snapshots, user_elo_ratings
--    and bots, iOS reads player_views and spectator_views. RLS stays enabled with
--    no policy, so a future grant still reads nothing.
-- 4. The JSONB columns go (name, deck_length, discard_pile_length, flipped,
--    players, power_suit, first_attacker, defender, table_battles,
--    elimination_order, good_timestamp, good_players), and writer_gen with them:
--    it fenced the legacy writers off the kernel's rows, and there are none.
--    The table title lives in the roster (Q5), so games.name goes too. Their
--    indexes go with them (idx_games_name explicitly, and
--    idx_games_playing_updated_at, which the heartbeat stopped using in 4b for
--    idx_games_bot_scan). `state` and `roster` become NOT NULL.
-- 5. commit_table and create_table lose their writer_gen writes (same signatures,
--    so CREATE OR REPLACE keeps their grants). The columns are still hex TEXT;
--    20260918130000_table_bytea.sql makes them BYTEA.
-- 6. delete_account stops rewriting games.players (Q8 as built in 4b): the
--    delete-account function redacts the name in every seated table itself,
--    through table_redact and commit_table, which also rewrites the cached views
--    this SQL never reached. What is left here is the leaderboard username.
-- 7. The lockdown loop, as after every change to a definer function.
--
-- No row's state, roster, version, status, needs_bots, round_epoch, seed, log or
-- updated_at changes (ALTER TABLE ... DROP COLUMN does not fire row triggers).
-- e2e/table_contract_migration.test.ts applies this on top of the captured
-- pre-4a rows, 4a and 4b's writes, and holds every row's blobs and envelopes to
-- what they were.

-- ---------------------------------------------------------------------------
-- 0. Refuse to run too early
-- ---------------------------------------------------------------------------
DO $$
DECLARE
  v_missing TEXT;
BEGIN
  IF EXISTS (SELECT 1 FROM games) AND NOT EXISTS (SELECT 1 FROM games WHERE writer_gen = 2) THEN
    RAISE EXCEPTION 'table contract: no games row is owned by the kernel writers (writer_gen 2), so the Phase 4b edge functions have not written here yet. Deploy and verify them first: this migration removes commit_game, create_game and games.players, which the functions before them use.'
      USING ERRCODE = 'object_not_in_prerequisite_state';
  END IF;

  SELECT string_agg(id, ', ' ORDER BY id) INTO v_missing FROM games WHERE state IS NULL OR roster IS NULL;
  IF v_missing IS NOT NULL THEN
    RAISE EXCEPTION 'table contract: games rows without a state or roster blob: %', v_missing
      USING ERRCODE = 'not_null_violation';
  END IF;
END $$;

-- ---------------------------------------------------------------------------
-- 1. The bridge
-- ---------------------------------------------------------------------------
DROP TRIGGER IF EXISTS games_legacy_bridge ON games;
DROP FUNCTION IF EXISTS legacy_games_bridge();
DROP FUNCTION IF EXISTS legacy_roster_hex(JSONB, TEXT, BOOLEAN);
DROP FUNCTION IF EXISTS legacy_lobby_state_hex(JSONB);
DROP FUNCTION IF EXISTS legacy_roster_field(BYTEA, INT, TEXT);
DROP FUNCTION IF EXISTS legacy_utf8_cut(BYTEA, INT);

-- ---------------------------------------------------------------------------
-- 2. The legacy writers, every overload
-- ---------------------------------------------------------------------------
DO $$
DECLARE
  fn regprocedure;
BEGIN
  FOR fn IN
    SELECT p.oid::regprocedure
    FROM pg_proc p
    JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'public'
      AND p.proname IN ('commit_game', 'create_game')
  LOOP
    EXECUTE format('DROP FUNCTION %s;', fn);
  END LOOP;
END $$;

-- ---------------------------------------------------------------------------
-- 3. No client privilege on games
-- ---------------------------------------------------------------------------
DROP POLICY IF EXISTS "Anyone can view games" ON games;
DROP POLICY IF EXISTS "Authenticated users can create games" ON games;
-- REVOKE on a table also revokes every column privilege on it.
REVOKE ALL ON public.games FROM PUBLIC, anon, authenticated;

-- ---------------------------------------------------------------------------
-- 4. The JSONB game shape
-- ---------------------------------------------------------------------------
DROP INDEX IF EXISTS idx_games_name;
DROP INDEX IF EXISTS idx_games_playing_updated_at;

ALTER TABLE games
  DROP COLUMN name,
  DROP COLUMN deck_length,
  DROP COLUMN discard_pile_length,
  DROP COLUMN flipped,
  DROP COLUMN players,
  DROP COLUMN power_suit,
  DROP COLUMN first_attacker,
  DROP COLUMN defender,
  DROP COLUMN table_battles,
  DROP COLUMN elimination_order,
  DROP COLUMN good_timestamp,
  DROP COLUMN good_players,
  DROP COLUMN writer_gen,
  ALTER COLUMN state SET NOT NULL,
  ALTER COLUMN roster SET NOT NULL;

COMMENT ON COLUMN games.state IS
  'The kernel''s durable board (c/src/table.h, v02) as \x-hex: every hand and the deck order, a lobby included. SENSITIVE: service_role only.';
COMMENT ON COLUMN games.roster IS
  'The kernel''s durable roster (c/src/roster.h, format 1, 1227 bytes) as \x-hex: each seat''s id, name and bot brain, and the table title. SENSITIVE: service_role only.';

-- ---------------------------------------------------------------------------
-- 5. The kernel writers, without writer_gen
-- ---------------------------------------------------------------------------
-- Every product comes from one table_commit_products call: the state and roster
-- blobs, the status as GAME_STATUS_* (0 waiting, 1 playing, 2 game_over, the
-- enum's order), needs_bots, the log records, the views. CAS on version, log
-- append or reset, round_epoch, membership, view cache; a lobby commit's human
-- list also prunes the humans who left, in the same transaction as the roster
-- that no longer seats them.
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
    status = v_status, state = '\x' || encode(decode(p_state, 'base64'), 'hex'), roster = COALESCE('\x' || encode(decode(p_roster, 'base64'), 'hex'), roster), needs_bots = p_needs_bots,
    game_seed = CASE WHEN v_status = 'waiting' THEN NULL ELSE COALESCE(p_game_seed, game_seed) END,
    logs_packed = CASE
      WHEN p_logs_reset THEN COALESCE(encode(decode(p_logs_packed, 'base64'), 'hex'), '')
      WHEN v_status = 'waiting' THEN ''
      ELSE COALESCE(logs_packed, '') || COALESCE(encode(decode(p_logs_packed, 'base64'), 'hex'), '')
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
    SELECT p_game_id, v.player_id, encode(decode(v.view, 'base64'), 'hex'), new_version, v_status::text, now()
    FROM unnest(p_view_players, p_views) AS v(player_id, view)
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
    DELETE FROM player_views
    WHERE game_id = p_game_id AND player_id <> ALL (p_view_players);
  END IF;

  IF p_spectator IS NOT NULL THEN
    INSERT INTO spectator_views (game_id, view, version, status, updated_at)
    VALUES (p_game_id, encode(decode(p_spectator, 'base64'), 'hex'), new_version, v_status::text, now())
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
    VALUES (p_game_id, 'waiting', '\x' || encode(decode(p_state, 'base64'), 'hex'), '\x' || encode(decode(p_roster, 'base64'), 'hex'), FALSE);

  INSERT INTO player_hands (game_id, player_id)
    VALUES (p_game_id, p_player_id);

  IF p_view IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    VALUES (p_game_id, p_player_id, encode(decode(p_view, 'base64'), 'hex'), 0, 'waiting', now())
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;

  IF p_spectator IS NOT NULL THEN
    INSERT INTO spectator_views (game_id, view, version, status, updated_at)
    VALUES (p_game_id, encode(decode(p_spectator, 'base64'), 'hex'), 0, 'waiting', now())
    ON CONFLICT (game_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;
END;
$$;

-- ---------------------------------------------------------------------------
-- 6. delete_account: the leaderboard copy of the username
-- ---------------------------------------------------------------------------
-- The seat names in games.roster and in every cached view are redacted by the
-- delete-account edge function before it calls this (table_redact per seated
-- table, committed through commit_table). Owned rows cascade from auth.users
-- when the function then deletes the user; this clears the denormalized
-- username in case that foreign key is ever SET NULL rather than CASCADE.
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

-- ---------------------------------------------------------------------------
-- 7. Grants: every SECURITY DEFINER function is service_role only
-- ---------------------------------------------------------------------------
-- 20260917000000 says why this runs after every change, not only after a CREATE.
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
    EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO service_role;', fn);
  END LOOP;
END $$;
