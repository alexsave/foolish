-- EXPAND: every games row carries the kernel's two durable blobs, while today's
-- server keeps working unchanged (docs/C_GAME_SHAPE_MIGRATION.md 3.4, Phase 4a).
--
-- The C game shape migration moves a games row to exactly `state` (the kernel's
-- durable board, unchanged v02) plus `roster` (the kernel's durable roster,
-- c/src/roster.h, ROSTER_FORMAT_VERSION 1, fixed 1227 bytes) plus the scalars the
-- kernel computes for SQL. It ships in three deploys so that no running edge
-- function ever meets a schema it cannot read:
--
--   4a expand   (this file)  add the columns, convert every row, keep them in step
--   4b switch   (functions)  the server reads and writes state + roster through C
--   4c contract (migration)  drop the JSONB columns and everything below marked legacy
--
-- WHAT THIS DOES
--
-- 1. games gains `roster` (\x-hex TEXT like `state`), `needs_bots` (the bot scan's
--    predicate as a column) and `writer_gen` (1 = today's JSONB writers own the
--    row, 2 = the kernel writers of 4b own it).
-- 2. Every existing row is converted in SQL, not in C (Q3): the roster from
--    `players` and `name`, and for every WAITING row a lobby `state` from
--    `players`. That also repairs the WAITING rows that still carry a finished
--    session's blob, which the kernel now refuses (GAME_INVALID_LOBBY_CARDS),
--    and a lobby's JSONB board is emptied (goods, trump, discard, table,
--    elimination) because today's server marshals it into that same kernel.
--    The conversion REFUSES what the kernel would refuse (an id over 36 bytes, a
--    name over 64 bytes, a title over 200 bytes, more than 8 seats, a duplicate
--    id, a bots row whose strategy_key is not a brain) so a bad row fails this
--    migration instead of loading as garbage later. A bot seat whose bots row
--    was deleted long ago is not one of those: a pre-pass (4.1) resolves those
--    seats first, per status, and the encoder stays strict behind it.
--    e2e/table_expand_migration.test.ts decodes every converted
--    row with the real C decoder and compares the envelopes C writes from it
--    with the ones today's server cached.
-- 3. A trigger keeps `roster`, `needs_bots` and the lobby `state` in step on
--    every write a legacy writer makes (commit_game, create_game, delete_account,
--    or any hand-run UPDATE of the JSONB roster). A trigger rather than edits to
--    each function because it is the one place every legacy write passes, and it
--    also catches a commit_game call that started under the old function body
--    and lands after this migration.
-- 4. commit_game refuses a row the kernel writers already own (writer_gen = 2),
--    so an in-flight old request that lost a CAS race to new code fails that one
--    request instead of overwriting the roster from stale JSON.
-- 5. commit_table and create_table, the kernel writers 4b calls. No caller yet.
-- 6. The bot scan's partial index on needs_bots.
--
-- Nothing a client can reach changes: the new columns are not in the games
-- column grant, and every function here is service_role only.

-- ---------------------------------------------------------------------------
-- 1. Columns
-- ---------------------------------------------------------------------------
ALTER TABLE games
  ADD COLUMN roster     TEXT,
  ADD COLUMN needs_bots BOOLEAN  NOT NULL DEFAULT FALSE,
  ADD COLUMN writer_gen SMALLINT NOT NULL DEFAULT 1;

COMMENT ON COLUMN games.roster IS
  'The kernel''s durable roster (c/src/roster.h, format 1, 1227 bytes) as \x-hex: each seat''s id, name and bot brain, and the table title. SENSITIVE like state (server-only, not in the column grant).';
COMMENT ON COLUMN games.needs_bots IS
  'PLAYING and a bot seat is still IN (the kernel''s table_needs_bots): the bot heartbeat''s scan predicate.';
COMMENT ON COLUMN games.writer_gen IS
  'Bridge-only, dropped by the contract migration. 1: the JSONB writers own this row and a trigger derives roster/needs_bots/lobby state from players. 2: the kernel writers (commit_table/create_table) own it and commit_game refuses it.';

-- ---------------------------------------------------------------------------
-- 2. The legacy conversion. Duplicates the byte layout of c/src/roster.h and of
--    view.c state_put for a lobby, once, for this bridge (Q3). Dropped in 4c.
-- ---------------------------------------------------------------------------

-- The longest prefix of whole UTF-8 scalars that fits p_max bytes: roster.c
-- roster_name_trim. The byte AT the budget is either the lead byte of the scalar
-- that would cross it (cut there) or inside that scalar (back off to its lead).
CREATE FUNCTION legacy_utf8_cut(p BYTEA, p_max INT)
RETURNS BYTEA
LANGUAGE plpgsql
IMMUTABLE
SET search_path = public
AS $$
DECLARE
  k INT := p_max;
BEGIN
  IF length(p) <= p_max THEN
    RETURN p;
  END IF;
  WHILE k > 0 AND (get_byte(p, k) & 192) = 128 LOOP
    k := k - 1;
  END LOOP;
  RETURN substring(p FROM 1 FOR k);
END;
$$;

-- One durable field: a length byte, the bytes, zero padding to p_cap.
CREATE FUNCTION legacy_roster_field(p BYTEA, p_cap INT, p_what TEXT)
RETURNS BYTEA
LANGUAGE plpgsql
IMMUTABLE
SET search_path = public
AS $$
BEGIN
  IF length(p) > p_cap THEN
    RAISE EXCEPTION 'legacy roster: % is % bytes, over the kernel''s cap of %', p_what, length(p), p_cap
      USING ERRCODE = 'string_data_right_truncation';
  END IF;
  RETURN set_byte('\x00'::bytea, 0, length(p)) || p || decode(repeat('00', p_cap - length(p)), 'hex');
END;
$$;

-- The durable roster (c/src/roster.h) of a JSONB players array and a title:
--
--   0    1        format = 1
--   1    1        n seats (0..8)
--   2    1+200    title: length byte, bytes, zero padded
--   203  8 x 128  seats, unused ones all zero:
--                   +0   1+36  id
--                   +37  1+64  name (UTF-8)
--                   +102 1+23  brain: bots.strategy_key for an is_ai seat, empty for a human
--                   +126 2     reserved, zero
--
-- Refuses, never clamps, unless p_trim: then a name over 64 bytes and a title
-- over 200 bytes are cut on a scalar boundary exactly as the kernel's own
-- writers cut them (roster_seat_add, table_create). Only the live trigger
-- trims, so a new long username can still join during the bridge; the backfill
-- of rows that already exist does not.
CREATE FUNCTION legacy_roster_hex(p_players JSONB, p_title TEXT, p_trim BOOLEAN DEFAULT FALSE)
RETURNS TEXT
LANGUAGE plpgsql
STABLE
SET search_path = public
AS $$
DECLARE
  c_suffix CONSTANT BYTEA := convert_to('''s Game', 'UTF8');
  v_title BYTEA := convert_to(COALESCE(p_title, ''), 'UTF8');
  v_n     INT;
  v_out   BYTEA;
  v_seat  JSONB;
  v_id    TEXT;
  v_name  BYTEA;
  v_brain TEXT;
  v_ids   TEXT[] := '{}';
BEGIN
  IF jsonb_typeof(p_players) IS DISTINCT FROM 'array' THEN
    RAISE EXCEPTION 'legacy roster: players is not a JSON array' USING ERRCODE = 'data_exception';
  END IF;
  v_n := jsonb_array_length(p_players);
  IF v_n > 8 THEN
    RAISE EXCEPTION 'legacy roster: % seats, over the kernel''s 8', v_n USING ERRCODE = 'data_exception';
  END IF;

  IF p_trim AND length(v_title) > 200 THEN
    -- table_create's rule: the creator's name cut to fit, then "'s Game".
    IF substring(v_title FROM length(v_title) - length(c_suffix) + 1) = c_suffix THEN
      v_title := legacy_utf8_cut(substring(v_title FROM 1 FOR length(v_title) - length(c_suffix)),
                                 200 - length(c_suffix)) || c_suffix;
    ELSE
      v_title := legacy_utf8_cut(v_title, 200);
    END IF;
  END IF;

  v_out := '\x01'::bytea || set_byte('\x00'::bytea, 0, v_n) || legacy_roster_field(v_title, 200, 'the title');

  FOR v_seat IN SELECT e FROM jsonb_array_elements(p_players) WITH ORDINALITY AS t(e, k) ORDER BY k LOOP
    v_id := v_seat->>'player_id';
    IF v_id IS NULL OR v_id = '' THEN
      RAISE EXCEPTION 'legacy roster: a seat has no player_id' USING ERRCODE = 'data_exception';
    END IF;
    IF v_id = ANY(v_ids) THEN
      RAISE EXCEPTION 'legacy roster: player % is seated twice', v_id USING ERRCODE = 'data_exception';
    END IF;
    v_ids := v_ids || v_id;
    IF jsonb_typeof(v_seat->'name') IS DISTINCT FROM 'string' THEN
      RAISE EXCEPTION 'legacy roster: seat % has no name', v_id USING ERRCODE = 'data_exception';
    END IF;
    v_name := convert_to(v_seat->>'name', 'UTF8');
    IF p_trim THEN
      v_name := legacy_utf8_cut(v_name, 64);
    END IF;

    IF jsonb_typeof(v_seat->'is_ai') IS DISTINCT FROM 'boolean' THEN
      RAISE EXCEPTION 'legacy roster: seat % has no is_ai flag', v_id USING ERRCODE = 'data_exception';
    ELSIF (v_seat->>'is_ai')::boolean THEN
      SELECT b.strategy_key INTO v_brain FROM bots b WHERE b.id::text = v_id;
      -- A brain is 1..23 bytes of printable ASCII (roster.c brain_ok). Whether
      -- this build links it is table_load's question (Q16), not SQL's. The
      -- backfill never meets the missing-row half of this: 4.1 resolves those
      -- seats before it runs. The trigger can, if a legacy writer ever seats a
      -- bot that is not in `bots`, and then the write is the thing to refuse.
      IF v_brain IS NULL OR v_brain !~ '^[!-~]{1,23}$' THEN
        RAISE EXCEPTION 'legacy roster: bot seat % has no bots row with a valid strategy_key (%)', v_id, v_brain
          USING ERRCODE = 'data_exception';
      END IF;
    ELSE
      v_brain := '';
    END IF;

    v_out := v_out
      || legacy_roster_field(convert_to(v_id, 'UTF8'), 36, format('seat %s''s id', v_id))
      || legacy_roster_field(v_name, 64, format('seat %s''s name', v_id))
      || legacy_roster_field(convert_to(v_brain, 'UTF8'), 23, format('seat %s''s brain', v_id))
      || '\x0000'::bytea;
  END LOOP;

  v_out := v_out || decode(repeat('00', 128 * (8 - v_n)), 'hex');
  IF length(v_out) <> 1227 THEN
    RAISE EXCEPTION 'legacy roster: encoded % bytes, not 1227', length(v_out);
  END IF;
  RETURN E'\\x' || encode(v_out, 'hex');
END;
$$;

-- The durable state blob of a lobby with these seats: [format 02][deterministic
-- deck 00] then view.c state_put, whose every count is zero in a lobby, so the
-- offsets are fixed:
--
--   status 00, n, power_suit 00, first_attacker 00, defender 00, discard 0000,
--   has_flipped 00, flip FE, good mask 00000000, has_ts 00, deck 0000, battles 00,
--   n x {status (idle 00, ready 01, in 02, out 03), awaiting 00, hand 00},
--   eliminated 00
--
-- No goods, no timestamp, no cards: GAME_INVALID_LOBBY_CARDS refuses any of them
-- in WAITING, so whatever a JSONB row still says about them is not carried.
CREATE FUNCTION legacy_lobby_state_hex(p_players JSONB)
RETURNS TEXT
LANGUAGE plpgsql
IMMUTABLE
SET search_path = public
AS $$
DECLARE
  v_n     INT;
  v_seats  BYTEA := '\x'::bytea;
  v_seat   JSONB;
  v_status INT;
BEGIN
  IF jsonb_typeof(p_players) IS DISTINCT FROM 'array' THEN
    RAISE EXCEPTION 'legacy lobby state: players is not a JSON array' USING ERRCODE = 'data_exception';
  END IF;
  v_n := jsonb_array_length(p_players);
  IF v_n > 8 THEN
    RAISE EXCEPTION 'legacy lobby state: % seats, over the kernel''s 8', v_n USING ERRCODE = 'data_exception';
  END IF;
  FOR v_seat IN SELECT e FROM jsonb_array_elements(p_players) WITH ORDINALITY AS t(e, k) ORDER BY k LOOP
    v_status := array_position(ARRAY['idle', 'ready', 'in', 'out'], v_seat->>'status');
    IF v_status IS NULL THEN
      RAISE EXCEPTION 'legacy lobby state: seat % has status %', v_seat->>'player_id', v_seat->>'status'
        USING ERRCODE = 'data_exception';
    END IF;
    v_seats := v_seats || set_byte('\x000000'::bytea, 0, v_status - 1);
  END LOOP;
  RETURN E'\\x' || encode(
    '\x020000'::bytea || set_byte('\x00'::bytea, 0, v_n)
      || '\x0000000000' || '\x00fe' || '\x00000000' || '\x00' || '\x0000' || '\x00'
      || v_seats || '\x00'::bytea,
    'hex');
END;
$$;

-- ---------------------------------------------------------------------------
-- 3. Keep a legacy-owned row's blobs in step with its JSONB roster
-- ---------------------------------------------------------------------------
-- Fires on every INSERT, and on every UPDATE that sets players, name or status
-- (commit_game sets all three on every commit; the lease RPCs set none). A row
-- the kernel writers own (writer_gen 2) is theirs: nothing is derived.
CREATE FUNCTION legacy_games_bridge()
RETURNS TRIGGER
LANGUAGE plpgsql
SET search_path = public
AS $$
BEGIN
  IF NEW.writer_gen <> 1 THEN
    RETURN NEW;
  END IF;
  NEW.roster := legacy_roster_hex(NEW.players, NEW.name, TRUE);
  NEW.needs_bots := NEW.status = 'playing' AND NEW.players @> '[{"is_ai": true, "status": "in"}]';
  -- Every row has both blobs, a lobby included. commit_game still writes NULL
  -- for a WAITING commit (so a finished session's blob never survives into the
  -- lobby); the lobby blob replaces it here. The JSONB board of a lobby is
  -- emptied the way game_reset_to_lobby empties the kernel's: today's server
  -- marshals it into the kernel for every lobby view, and the kernel refuses a
  -- lobby holding goods, a trump, a discard pile, a table or an elimination.
  IF NEW.status = 'waiting' THEN
    NEW.state := legacy_lobby_state_hex(NEW.players);
    NEW.deck_length := 0;
    NEW.discard_pile_length := 0;
    NEW.flipped := NULL;
    NEW.power_suit := 0;
    NEW.first_attacker := 0;
    NEW.defender := 0;
    NEW.table_battles := '[]';
    NEW.elimination_order := '[]';
    NEW.good_players := '[]';
    NEW.good_timestamp := NULL;
  END IF;
  RETURN NEW;
END;
$$;

CREATE TRIGGER games_legacy_bridge
  BEFORE INSERT OR UPDATE OF players, name, status ON games
  FOR EACH ROW
  EXECUTE FUNCTION legacy_games_bridge();

-- ---------------------------------------------------------------------------
-- 4. Convert every existing row
-- ---------------------------------------------------------------------------
-- After the trigger, so a legacy commit that lands while this runs derives its
-- own blobs. updated_at is left alone: the bot heartbeat scans a window of it,
-- and stamping every row now would make every abandoned game look live.
-- `version` is left alone too: nothing a client or a CAS holds changes meaning.
ALTER TABLE games DISABLE TRIGGER update_games_updated_at;
-- AND the bridge, across the pre-pass only. The pre-pass rewrites games.players,
-- which is exactly what the bridge fires on, so it would re-derive the roster
-- from a HALF-resolved row: a lobby with two dead bot seats raises on the second
-- while the first is being dropped, which is how the first deploy of this
-- migration failed on hosted. Nothing is lost by holding it off - 4.2 below
-- rewrites roster, needs_bots and the lobby state for EVERY row a moment later,
-- which is all the bridge would have computed. It goes back on before section 5,
-- so the legacy writers still derive their own blobs afterwards.
ALTER TABLE games DISABLE TRIGGER games_legacy_bridge;

-- 4.1 THE PRE-PASS: bot seats whose bots row is gone.
--
-- A seat can name a bot that no longer exists. 20260711130000_drop_non_wasm_bots
-- deleted the retired strategies (champion, ultimate_champion, hacker, espresso,
-- semtex, semtex_max) from `bots` and left behind every seat in `games.players`
-- that named one. The hosted database has 29 such seats across 21 games: 20 of
-- them waiting, 9 game_over, none playing. The encoder below is right to refuse
-- a bot seat with no brain when it converts a row the server just wrote, but as
-- a deploy gate over six-month-old rows it is wrong, and it is what failed this
-- migration on hosted. So the orphans are resolved here and the encoder stays
-- strict: a seat with no id or no name, a name over the cap, more than 8 seats,
-- a duplicate id or a bots row whose strategy_key is not a brain all still stop
-- the migration.
--
-- The resolution rewrites `games.players`, not just the roster bytes, because
-- the bridge trigger re-derives the roster from `players` on every later legacy
-- write: a "continue" on one of these finished games has to keep working, and a
-- fix that lived only in the backfill would raise the moment one was touched.
--
--   waiting    the seat is dropped. The bot does not exist any more, so an
--              abandoned lobby simply loses a ghost that could never play; a
--              player who opens it can add a bot that does exist. Its bot_hands
--              row goes with it (the bot_hands -> bots FK already cascaded it
--              away when the bots row was deleted, so the DELETE is belt and
--              braces rather than the thing doing the work).
--
--   game_over  the seat, its name and its place stay, and it is repointed at a
--              bots row that survives, so the record still reads "%Champion 1"
--              while the roster names a brain the kernel links. The brain
--              cannot affect the row: the game is finished, the state blob is
--              the whole record of it, and nothing will ever ask that seat for
--              a move again.
--
--   playing    the same repoint, and the bot_hands membership follows it.
--              Dropping the seat is not open here (its cards are in the state
--              blob, whose seat count table_load checks against the roster's),
--              and the game is frozen today anyway because the bot loop cannot
--              resolve the bot either, so a brain that exists is strictly
--              better than what the row has now. No such row exists as of this
--              deploy; this is the branch for one appearing in between.
--
-- The substitute is a `random` bot. It is the one brain that claims nothing
-- about how it plays, so it reads as the placeholder it is instead of falsely
-- crediting the seat to a strategy it never used. Whichever surviving `random`
-- bot is not already seated in that game is taken (a seat cannot be filled
-- twice), lowest id first, so the choice is deterministic; if the game seats
-- every one of them the pick widens to any surviving bot, and if even that is
-- empty the migration raises rather than guess.
DO $$
DECLARE
  c_substitute_key CONSTANT TEXT := 'random';
  r     RECORD;
  v_sub UUID;
  v_key TEXT;
BEGIN
  FOR r IN
    SELECT g.id, g.status::text AS status, (s.k - 1)::int AS idx,
           s.seat->>'player_id' AS bot_id, s.seat->>'name' AS bot_name
    FROM games g
    CROSS JOIN LATERAL jsonb_array_elements(g.players) WITH ORDINALITY AS s(seat, k)
    WHERE jsonb_typeof(s.seat->'is_ai') = 'boolean'
      AND (s.seat->>'is_ai')::boolean
      AND s.seat->>'player_id' IS NOT NULL
      AND NOT EXISTS (SELECT 1 FROM bots b WHERE b.id::text = s.seat->>'player_id')
    -- Descending, so dropping a seat never shifts the index of one still to come.
    ORDER BY g.id, s.k DESC
  LOOP
    IF r.status = 'waiting' THEN
      UPDATE games SET players = players - r.idx WHERE id = r.id;
      DELETE FROM bot_hands WHERE game_id = r.id AND bot_id::text = r.bot_id;
      RAISE NOTICE 'table_expand: game % (waiting) drops seat % "%": its bot % has no bots row',
        r.id, r.idx, r.bot_name, r.bot_id;
    ELSE
      -- Re-read `games` per seat: an earlier iteration of this same loop may
      -- already have seated a substitute in this very game.
      SELECT b.id, b.strategy_key INTO v_sub, v_key
      FROM bots b
      WHERE b.strategy_key ~ '^[!-~]{1,23}$'
        AND NOT EXISTS (
          SELECT 1 FROM games g2
          CROSS JOIN LATERAL jsonb_array_elements(g2.players) AS s2(seat)
          WHERE g2.id = r.id AND s2.seat->>'player_id' = b.id::text)
      ORDER BY (b.strategy_key <> c_substitute_key), b.id
      LIMIT 1;
      IF v_sub IS NULL THEN
        RAISE EXCEPTION 'legacy roster: game % seat % (bot %) has no bots row, and no surviving bot is free to stand in for it',
          r.id, r.idx, r.bot_id USING ERRCODE = 'data_exception';
      END IF;
      UPDATE games SET players = jsonb_set(players, ARRAY[r.idx::text, 'player_id'], to_jsonb(v_sub::text))
      WHERE id = r.id;
      IF r.status = 'playing' THEN
        DELETE FROM bot_hands WHERE game_id = r.id AND bot_id::text = r.bot_id;
        INSERT INTO bot_hands (game_id, bot_id) VALUES (r.id, v_sub) ON CONFLICT DO NOTHING;
      END IF;
      RAISE NOTICE 'table_expand: game % (%) keeps seat % "%" and gives it bot % (%): its own bot % has no bots row',
        r.id, r.status, r.idx, r.bot_name, v_sub, v_key, r.bot_id;
    END IF;
  END LOOP;
END $$;

UPDATE games SET
  roster     = legacy_roster_hex(players, name),
  needs_bots = status = 'playing' AND players @> '[{"is_ai": true, "status": "in"}]',
  state      = CASE WHEN status = 'waiting' THEN legacy_lobby_state_hex(players) ELSE state END;

-- A lobby's JSONB board, emptied as the bridge trigger empties it (see there):
-- rows continued before handleContinue cleared goods still hold them, and
-- today's server marshals them into a kernel that refuses them.
UPDATE games SET
  deck_length = 0, discard_pile_length = 0, flipped = NULL, power_suit = 0, first_attacker = 0, defender = 0,
  table_battles = '[]', elimination_order = '[]', good_players = '[]', good_timestamp = NULL
WHERE status = 'waiting';

ALTER TABLE games ENABLE TRIGGER games_legacy_bridge;
ALTER TABLE games ENABLE TRIGGER update_games_updated_at;

-- ---------------------------------------------------------------------------
-- 5. commit_game refuses a row the kernel writers own
-- ---------------------------------------------------------------------------
-- The shipped body (20260906120000) with one change: the version fence also
-- requires writer_gen = 1, and a miss on a writer_gen 2 row is an error rather
-- than a conflict (a conflict would reload JSONB the kernel writers no longer
-- keep, and retry forever). Same signature, so CREATE OR REPLACE keeps its grants.
CREATE OR REPLACE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_seats            JSONB   DEFAULT NULL,
  p_bot_seats        JSONB   DEFAULT NULL,
  p_state            TEXT    DEFAULT NULL,
  p_logs_packed      TEXT    DEFAULT NULL,
  p_logs_reset       BOOLEAN DEFAULT FALSE,
  p_game_seed        TEXT    DEFAULT NULL,
  p_views            JSONB   DEFAULT NULL,
  p_spectator        TEXT    DEFAULT NULL,
  p_closed_round     BOOLEAN DEFAULT FALSE
) RETURNS JSONB
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_new_version BIGINT;
  v_round_epoch BIGINT;
  g games%ROWTYPE;
BEGIN
  g := jsonb_populate_record(NULL::games, p_game);

  UPDATE games SET
    name = g.name, deck_length = g.deck_length, discard_pile_length = g.discard_pile_length,
    flipped = g.flipped, players = g.players, status = g.status, power_suit = g.power_suit,
    first_attacker = g.first_attacker, defender = g.defender, table_battles = g.table_battles,
    elimination_order = g.elimination_order, good_timestamp = g.good_timestamp,
    good_players = g.good_players,
    -- A WAITING commit never keeps a finished session's blob; the bridge
    -- trigger writes the lobby blob from g.players in its place.
    state = CASE WHEN g.status = 'waiting' THEN NULL ELSE COALESCE(p_state, state) END,
    game_seed = CASE WHEN g.status = 'waiting' THEN NULL ELSE COALESCE(p_game_seed, game_seed) END,
    logs_packed = CASE
      WHEN p_logs_reset THEN COALESCE(p_logs_packed, '')
      WHEN g.status = 'waiting' THEN ''
      ELSE COALESCE(logs_packed, '') || COALESCE(p_logs_packed, '')
    END,
    round_epoch = CASE
      WHEN p_logs_reset OR g.status = 'waiting' THEN 0
      WHEN p_closed_round THEN version + 1
      ELSE round_epoch
    END,
    updated_at = now(), version = version + 1
  WHERE id = p_game_id AND version = p_expected_version AND writer_gen = 1
  RETURNING version, round_epoch INTO v_new_version, v_round_epoch;

  IF NOT FOUND THEN
    IF EXISTS (SELECT 1 FROM games WHERE id = p_game_id AND writer_gen <> 1) THEN
      RAISE EXCEPTION 'commit_game: game % is owned by the table writers (writer_gen 2); reload it through them', p_game_id
        USING ERRCODE = 'object_not_in_prerequisite_state';
    END IF;
    RETURN jsonb_build_object('status', 'conflict');
  END IF;

  IF p_seats IS NOT NULL THEN
    INSERT INTO player_hands (game_id, player_id)
    SELECT p_game_id, s::uuid FROM jsonb_array_elements_text(p_seats) AS s
    ON CONFLICT (game_id, player_id) DO UPDATE SET updated_at = now();
  END IF;

  IF p_bot_seats IS NOT NULL THEN
    IF jsonb_array_length(p_bot_seats) > 0 THEN
      INSERT INTO bot_hands (game_id, bot_id)
      SELECT p_game_id, b::uuid FROM jsonb_array_elements_text(p_bot_seats) AS b
      ON CONFLICT (game_id, bot_id) DO UPDATE SET updated_at = now();
    END IF;

    DELETE FROM bot_hands
    WHERE game_id = p_game_id
      AND bot_id NOT IN (
        SELECT b::uuid FROM jsonb_array_elements_text(p_bot_seats) AS b
      );
  END IF;

  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, (v->>'player_id')::uuid, v->>'view', v_new_version, v->>'status', now()
    FROM jsonb_array_elements(p_views) AS v
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();

    DELETE FROM player_views
    WHERE game_id = p_game_id
      AND player_id NOT IN (
        SELECT (v->>'player_id')::uuid FROM jsonb_array_elements(p_views) AS v
      );
  END IF;

  IF p_spectator IS NOT NULL THEN
    INSERT INTO spectator_views (game_id, view, version, status, updated_at)
    VALUES (p_game_id, p_spectator, v_new_version, g.status, now())
    ON CONFLICT (game_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;

  RETURN jsonb_build_object('status', 'ok', 'version', v_new_version, 'round_epoch', v_round_epoch);
END;
$$;

-- ---------------------------------------------------------------------------
-- 6. The kernel writers (3.3). Called from Phase 4b on.
-- ---------------------------------------------------------------------------
-- Every product comes from one table_commit_products call: the state and roster
-- blobs, the status as GAME_STATUS_* (0 waiting, 1 playing, 2 game_over, the
-- enum's order), needs_bots, the log records, the views. The rules are
-- commit_game's (CAS on version, log append or reset, round_epoch, membership,
-- view cache), and a lobby commit's human list also prunes the humans who left,
-- in the same transaction as the roster that no longer seats them.
--
-- The blobs ride as base64 (measured through PostgREST: a third fewer body bytes
-- than hex, the same latency), and the columns stay hex TEXT until
-- 20260918220000_table_bytea.sql makes them BYTEA, so this body writes the text
-- forms the legacy writers write. The signature is final from here on: the
-- functions deployed between 4a and 4c already call it. The views are parallel
-- arrays with the game's status, the roster NULL when the operation left it alone,
-- and the result the OUT columns (committed, new_version, new_round_epoch).
CREATE FUNCTION commit_table(
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
    status = v_status, state = '\x' || encode(decode(p_state, 'base64'), 'hex'), roster = COALESCE('\x' || encode(decode(p_roster, 'base64'), 'hex'), roster), needs_bots = p_needs_bots, writer_gen = 2,
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

-- A new lobby from table_create's products, owned by the kernel writers.
CREATE FUNCTION create_table(
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

  INSERT INTO games (id, status, state, roster, needs_bots, writer_gen)
    VALUES (p_game_id, 'waiting', '\x' || encode(decode(p_state, 'base64'), 'hex'), '\x' || encode(decode(p_roster, 'base64'), 'hex'), FALSE, 2);

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
-- 7. The bot scan's index
-- ---------------------------------------------------------------------------
CREATE INDEX idx_games_bot_scan ON games(updated_at) WHERE needs_bots;

-- ---------------------------------------------------------------------------
-- 8. Grants: service_role only
-- ---------------------------------------------------------------------------
-- commit_table and create_table are new SECURITY DEFINER functions, so they
-- arrive with EXECUTE for PUBLIC, anon and authenticated (20260917000000 says
-- why that matters). The legacy helpers are not definers, but no client has a
-- reason to call them either.
DO $$
DECLARE
  fn regprocedure;
BEGIN
  FOR fn IN
    SELECT p.oid::regprocedure
    FROM pg_proc p
    JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'public'
      AND p.prorettype <> 'trigger'::regtype
      AND (p.prosecdef OR p.proname LIKE 'legacy\_%')
  LOOP
    EXECUTE format('REVOKE ALL ON FUNCTION %s FROM PUBLIC, anon, authenticated;', fn);
    EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO service_role;', fn);
  END LOOP;
END $$;
