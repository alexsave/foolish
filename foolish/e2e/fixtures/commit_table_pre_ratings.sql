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
