-- BYTEA: the kernel's blobs are stored as bytes, and the last JSON in the game
-- tables goes (docs/C_GAME_SHAPE_MIGRATION.md 3.1, "Final pass: BYTEA").
--
-- Deploy with 20260918120000_table_contract.sql or any time after it: the
-- functions live by then already call commit_table and create_table with the
-- signature this keeps (base64 blobs, parallel view arrays, the OUT result), and
-- every reader they have parses both text forms a SELECT returns ('\x'-hex from a
-- BYTEA column through PostgREST, hex from the TEXT column before). The web and
-- iOS read player_views.view and spectator_views.view the same way: both strip an
-- optional '\x' before decoding hex.
--
-- WHAT THIS DOES
--
-- 0. Refuses, naming the rows, if any blob is not whole hex bytes (with or without
--    the '\x' prefix) or any snapshot seat id is not a UUID, so a bad row fails
--    this migration instead of converting to garbage.
-- 1. Converts in place, each value by its own form: '\x'-hex (games.state,
--    games.roster, as the kernel writers and the legacy writers wrote them) and
--    bare hex (games.logs_packed, player_views.view, spectator_views.view) both
--    decode to the same bytes. An empty log stays empty; NULL stays NULL.
--    games.game_seed stays hex TEXT: the kernel takes it as text.
-- 2. game_snapshots.player_ids JSONB -> UUID[] (seat order kept), its GIN index
--    and the participants' read policy rebuilt on the array (@>, which the GIN
--    index serves).
-- 3. commit_table and create_table write bytes (same signatures, so CREATE OR
--    REPLACE keeps their grants).
-- 4. The lockdown loop, as after every change to a definer function.
--
-- ALTER COLUMN ... TYPE rewrites the table and fires no row trigger: updated_at,
-- version and every other column keep their values.
-- e2e/table_bytea_migration.test.ts applies this over the captured rows and holds
-- every blob byte for byte, and every envelope C writes from the converted rows.

-- ---------------------------------------------------------------------------
-- 0. Refuse what does not convert
-- ---------------------------------------------------------------------------
DO $$
DECLARE
  v_bad TEXT;
BEGIN
  SELECT string_agg(what, ', ' ORDER BY what) INTO v_bad FROM (
    SELECT 'games ' || id || ' ' || col AS what
    FROM games, LATERAL (VALUES ('state', state), ('roster', roster), ('logs_packed', logs_packed)) AS b(col, val)
    WHERE val IS NOT NULL AND val !~ '^(\\x)?([0-9a-fA-F]{2})*$'
    UNION ALL
    SELECT 'player_views ' || game_id || '/' || player_id FROM player_views WHERE view !~ '^(\\x)?([0-9a-fA-F]{2})*$'
    UNION ALL
    SELECT 'spectator_views ' || game_id FROM spectator_views WHERE view !~ '^(\\x)?([0-9a-fA-F]{2})*$'
    UNION ALL
    SELECT 'game_snapshots ' || id FROM game_snapshots
    WHERE jsonb_typeof(player_ids) <> 'array'
       OR EXISTS (SELECT 1 FROM jsonb_array_elements(player_ids) AS e
                  WHERE jsonb_typeof(e) <> 'string'
                     OR e #>> '{}' !~* '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$')
  ) AS bad;
  IF v_bad IS NOT NULL THEN
    RAISE EXCEPTION 'table bytea: these values are not whole hex bytes or UUID seat lists: %', v_bad
      USING ERRCODE = 'invalid_text_representation';
  END IF;
END $$;

-- The one conversion every blob column uses: the bytes of a hex text value, with
-- or without the '\x' prefix. pg_temp: gone when this migration's session ends.
CREATE FUNCTION pg_temp.hex_bytes(p TEXT) RETURNS BYTEA
LANGUAGE sql IMMUTABLE STRICT
AS $fn$ SELECT decode(CASE WHEN left(p, 2) = '\x' THEN substr(p, 3) ELSE p END, 'hex') $fn$;

CREATE FUNCTION pg_temp.jsonb_uuids(p JSONB) RETURNS UUID[]
LANGUAGE sql IMMUTABLE STRICT
AS $fn$ SELECT coalesce(array_agg(e::uuid ORDER BY ord), '{}') FROM jsonb_array_elements_text(p) WITH ORDINALITY AS t(e, ord) $fn$;

-- ---------------------------------------------------------------------------
-- 1. The blobs
-- ---------------------------------------------------------------------------
ALTER TABLE games
  ALTER COLUMN state TYPE BYTEA USING pg_temp.hex_bytes(state),
  ALTER COLUMN roster TYPE BYTEA USING pg_temp.hex_bytes(roster),
  ALTER COLUMN logs_packed TYPE BYTEA USING pg_temp.hex_bytes(logs_packed);

ALTER TABLE player_views ALTER COLUMN view TYPE BYTEA USING pg_temp.hex_bytes(view);
ALTER TABLE spectator_views ALTER COLUMN view TYPE BYTEA USING pg_temp.hex_bytes(view);

COMMENT ON COLUMN games.state IS
  'The kernel''s durable board (c/src/table.h, v02): every hand and the deck order, a lobby included. SENSITIVE: service_role only.';
COMMENT ON COLUMN games.roster IS
  'The kernel''s durable roster (c/src/roster.h, format 1, 1227 bytes): each seat''s id, name and bot brain, and the table title. SENSITIVE: service_role only.';
COMMENT ON COLUMN games.logs_packed IS
  'The session log: kernel log records with u48 timestamps, DRAW identities pre-masked, appended by commit_table. Empty in a lobby and once the replay snapshot is stored.';

-- ---------------------------------------------------------------------------
-- 2. Snapshot seats
-- ---------------------------------------------------------------------------
DROP POLICY IF EXISTS "Participants can read snapshots" ON game_snapshots;
DROP INDEX IF EXISTS idx_game_snapshots_player_ids;
ALTER TABLE game_snapshots
  ALTER COLUMN player_ids DROP DEFAULT,
  ALTER COLUMN player_ids TYPE UUID[] USING pg_temp.jsonb_uuids(player_ids),
  ALTER COLUMN player_ids SET DEFAULT '{}';
CREATE INDEX idx_game_snapshots_player_ids ON game_snapshots USING GIN (player_ids);
CREATE POLICY "Participants can read snapshots" ON game_snapshots
  FOR SELECT USING (
    (select auth.role()) = 'service_role'
    OR player_ids @> ARRAY[(select auth.uid())]
  );

-- ---------------------------------------------------------------------------
-- 3. The kernel writers, writing bytes
-- ---------------------------------------------------------------------------
-- Every product comes from one table_commit_products call: the state and roster
-- blobs, the status as GAME_STATUS_*, needs_bots, the log records, the views.
-- CAS on version, log append or reset, round_epoch, membership, view cache; a
-- lobby commit's human list also prunes the humans who left, in the same
-- transaction as the roster that no longer seats them.
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

-- ---------------------------------------------------------------------------
-- 4. Grants: every SECURITY DEFINER function is service_role only
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
