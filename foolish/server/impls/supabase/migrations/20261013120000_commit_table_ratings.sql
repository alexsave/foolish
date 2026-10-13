-- commit_table learns to rate: three new arguments, all defaulted.
--
-- A finished game's rating changes are about to be applied by the commit that
-- ends the game instead of by the edge function after it (seed.sql has the
-- reasoning, on the function). This is the database half of the EXPAND step:
-- the function takes the new arguments and nothing sends them yet, so every
-- caller deployed today - fifteen named arguments - runs exactly the statements
-- it ran before.
--
-- A DROP, THEN THE CREATE, and not CREATE OR REPLACE alone. Postgres identifies
-- a function by its argument TYPES, so "replacing" commit_table with a longer
-- argument list creates a second function beside the first, and every call that
-- names only the old arguments is then ambiguous between the two ("could not
-- choose the best candidate function"): every move in every game would fail.
-- Both statements run in this one transaction, so no call ever sees zero
-- functions or two.
--
-- A DROPPED AND RE-CREATED FUNCTION GETS FRESH DEFAULT GRANTS, which on Supabase
-- means anon and authenticated may execute it. That is how commit_game reopened
-- on the hosted project once (seed.sql, the SECURITY note above its grant
-- loop). So the last statement here locks it again, the way that loop does.
--
-- The function is seed.sql's, character for character
-- (e2e/validation/migration_guard_validation.test.ts holds the two equal).
--
-- ONE GUARDED BLOCK, per migrations/README.md: `supabase start` and `db reset`
-- apply migrations BEFORE seed.sql, and seed.sql is the schema, so on every
-- database except hosted this would run before `games` exists. Everywhere else
-- seed.sql creates this function itself.

DO $$
BEGIN
    IF to_regclass('public.games') IS NULL THEN
        RAISE NOTICE 'commit_table_ratings: no games table here, so this database is built from seed.sql, which already holds this function - nothing to migrate';
        RETURN;
    END IF;

    DROP FUNCTION IF EXISTS public.commit_table(TEXT, BIGINT, TEXT, SMALLINT, BOOLEAN, TEXT, UUID[], UUID[], TEXT, BOOLEAN, TEXT, UUID[], TEXT[], TEXT, BOOLEAN);

    EXECUTE $ddl$
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
  p_rated_ids        UUID[]    DEFAULT NULL, -- the seats this commit RATES, humans and bots; NULL: it rates nobody
  p_rated_bots       BOOLEAN[] DEFAULT NULL, -- parallel: the seat is a bot (its rating is a bots row)
  p_rated_deltas     INTEGER[] DEFAULT NULL, -- parallel: the kernel's change to that seat's rating
  OUT committed       BOOLEAN,              -- FALSE: the version fence refused (another writer committed first)
  OUT new_version     BIGINT,
  OUT new_round_epoch BIGINT
)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $commit_table$
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
  IF (p_rated_ids IS NULL) <> (p_rated_bots IS NULL) OR (p_rated_ids IS NULL) <> (p_rated_deltas IS NULL)
     OR cardinality(p_rated_ids) <> cardinality(p_rated_bots) OR cardinality(p_rated_ids) <> cardinality(p_rated_deltas) THEN
    RAISE EXCEPTION 'commit_table: p_rated_ids, p_rated_bots and p_rated_deltas go together, one of each per seat' USING ERRCODE = 'invalid_parameter_value';
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

  -- THE RATINGS, in the transaction that ends the game and behind its version
  -- fence, so a game changes them exactly once: the commit that loses the fence
  -- returned above, and a retry that wins it is the only one that gets here.
  --
  -- Each change is ADDED to the row as it stands now. The kernel worked the
  -- changes out from ratings read a moment before this commit, and a seat that
  -- sits at two tables (every bot does) may have finished the other one since:
  -- writing "the rating I read, plus my change" would erase that game, which is
  -- what the edge function used to do. The game's own record of what it did is
  -- the stamp in its state blob (c/src/view.h GameRatings); these rows are only
  -- the running totals.
  --
  -- Rows are locked in id order, humans before bots, so two games ending at
  -- once with seats in common wait for each other instead of deadlocking.
  -- previous_elo is kept for the web bundles that still read it.
  IF p_rated_ids IS NOT NULL THEN
    -- A human with no row yet starts from the column defaults. A seat whose
    -- account is gone (delete_account redacts the roster, the id stays) has no
    -- auth.users row to hang a rating on, and is skipped rather than failing
    -- the commit that ends the game.
    INSERT INTO user_elo_ratings (user_id)
    SELECT r.id FROM unnest(p_rated_ids, p_rated_bots) AS r(id, bot)
    WHERE NOT r.bot AND EXISTS (SELECT 1 FROM auth.users a WHERE a.id = r.id)
    ON CONFLICT (user_id) DO NOTHING;

    PERFORM 1 FROM user_elo_ratings WHERE user_id = ANY (p_rated_ids) ORDER BY user_id FOR UPDATE;
    PERFORM 1 FROM bots WHERE id = ANY (p_rated_ids) ORDER BY id FOR UPDATE;

    UPDATE user_elo_ratings u SET
      previous_elo = u.elo_rating, elo_rating = GREATEST(0, u.elo_rating + r.delta),
      games_played = u.games_played + 1, updated_at = now()
    FROM unnest(p_rated_ids, p_rated_bots, p_rated_deltas) AS r(id, bot, delta)
    WHERE NOT r.bot AND u.user_id = r.id;

    UPDATE bots b SET
      previous_elo = b.elo_rating, elo_rating = GREATEST(0, b.elo_rating + r.delta),
      games_played = b.games_played + 1, updated_at = now()
    FROM unnest(p_rated_ids, p_rated_bots, p_rated_deltas) AS r(id, bot, delta)
    WHERE r.bot AND b.id = r.id;
  END IF;
END;
$commit_table$;
$ddl$;

    REVOKE ALL ON FUNCTION public.commit_table(TEXT, BIGINT, TEXT, SMALLINT, BOOLEAN, TEXT, UUID[], UUID[], TEXT, BOOLEAN, TEXT, UUID[], TEXT[], TEXT, BOOLEAN, UUID[], BOOLEAN[], INTEGER[]) FROM PUBLIC, anon, authenticated;
    GRANT EXECUTE ON FUNCTION public.commit_table(TEXT, BIGINT, TEXT, SMALLINT, BOOLEAN, TEXT, UUID[], UUID[], TEXT, BOOLEAN, TEXT, UUID[], TEXT[], TEXT, BOOLEAN, UUID[], BOOLEAN[], INTEGER[]) TO service_role;
END $$;
