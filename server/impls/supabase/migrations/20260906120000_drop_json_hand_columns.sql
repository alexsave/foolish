-- Drop the JSONB hand/deck columns. They were write-only.
--
-- games.state has been the authoritative dealt state since 20260707120000: a
-- dealt load reconstructs every hand, the deck, the battles and the statuses
-- from the packed kernel blob and never looks at these columns. Nothing else
-- read them either. The only SELECT of player_hands.hand, bot_hands.hand or
-- game_decks.deck in the tree is loadCompleteGame's join, and there the values
-- reach a `players` array whose hands the blob path immediately discards; a
-- WAITING game has no cards to carry, so the lobby path read '[]' and built a
-- lobby from it.
--
-- So commit_game has been serializing every hand to JSON and writing it, on
-- every move, into columns no reader has consulted since the blob landed.
--
-- WHAT SURVIVES, AND WHY THE TABLES DO NOT GO WITH THEIR COLUMNS.
-- player_hands is the membership table: it is what the realtime RLS policies
-- ask "is this user in this game" (seed.sql's gu-* and chat: broadcast policies
-- both EXISTS over it) and what its own dashboard SELECT policy gates on.
-- bot_hands is the lobby's bot roster, and commit_game's prune of it is how
-- removing a lobby bot takes effect without a second round-trip. Both keep
-- their keys and lose only the payload.
--
-- game_decks has no such second job. It was written and deleted and never
-- read, so the table goes.
--
-- The RPC signatures change, so commit_game is DROPped rather than REPLACEd:
-- p_deck disappears, and p_hands / p_bot_hands become UUID lists because
-- membership is all they ever conveyed that anything consumed.

-- ---------------------------------------------------------------------------
-- 1. The columns and the table
-- ---------------------------------------------------------------------------
ALTER TABLE player_hands DROP COLUMN IF EXISTS hand;
ALTER TABLE player_hands DROP COLUMN IF EXISTS awaiting_attack;
ALTER TABLE bot_hands    DROP COLUMN IF EXISTS hand;
ALTER TABLE bot_hands    DROP COLUMN IF EXISTS awaiting_attack;
DROP TABLE IF EXISTS game_decks CASCADE;

COMMENT ON TABLE player_hands IS
  'Membership: which humans are in which game. Named for the hands it used to carry; the dealt state is games.state. The realtime RLS policies EXISTS over this table, so it is load-bearing even though it holds no cards.';
COMMENT ON TABLE bot_hands IS
  'Membership: which bots are in which game. See player_hands.';

-- ---------------------------------------------------------------------------
-- 2. commit_game, membership instead of payload
--
-- The body below is the shipped one with three blocks replaced; everything else
-- - the version fence, the WAITING reset that clears state/game_seed/
-- logs_packed, the round_epoch rule, the player_views cache and its prune - is
-- exactly what it was. The signature changes, so this is a DROP and a CREATE
-- rather than a REPLACE.
-- ---------------------------------------------------------------------------
DROP FUNCTION IF EXISTS commit_game(TEXT, BIGINT, JSONB, JSONB, JSONB, JSONB, TEXT, TEXT, BOOLEAN, TEXT, JSONB, TEXT, BOOLEAN);

CREATE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_seats            JSONB   DEFAULT NULL,  -- human members ["uuid",...]; NULL leaves player_hands untouched (a dealt commit cannot change the roster)
  p_bot_seats        JSONB   DEFAULT NULL,  -- bot members ["uuid",...]; NULL leaves bot_hands untouched, [] prunes every bot
  p_state            TEXT    DEFAULT NULL,  -- packed kernel state blob (hex); NULL leaves the column unchanged (never-dealt games)
  p_logs_packed      TEXT    DEFAULT NULL,  -- this move's logwire records (bare hex), appended under the version fence
  p_logs_reset       BOOLEAN DEFAULT FALSE, -- session reset (GAME_START in the records): replace instead of append
  p_game_seed        TEXT    DEFAULT NULL,  -- deal seed (hex); set once at the deal, NULL on every other commit leaves it unchanged
  p_views            JSONB   DEFAULT NULL,  -- per-participant masked view cache rows [{player_id,view,status}]; NULL leaves player_views untouched
  p_spectator        TEXT    DEFAULT NULL,  -- fully-masked seat -1 spectator view (bare hex); NULL leaves spectator_views untouched
  p_closed_round     BOOLEAN DEFAULT FALSE  -- TRUE when THIS move closed a round (pickup/discard): stamp round_epoch with the new version so the round-boundary guard rejects moves composed against the prior round (REJECT_STALE_ROUND, docs/WEB_RACE_BUG_HANDOFF.md)
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
    -- A WAITING commit is the `continue` reset (or a lobby op): the finished
    -- session's volatile state must NOT survive into the new lobby. A stale
    -- blob desyncs from the mutable lobby roster (seat-count mismatches brick
    -- every subsequent load) and leaks the previous session's hands through
    -- the blob-authoritative loaders. COALESCE alone never cleared it.
    state = CASE WHEN g.status = 'waiting' THEN NULL ELSE COALESCE(p_state, state) END,
    -- Same discipline as `state`: a WAITING reset clears the finished session's
    -- seed so the next deal writes a fresh one; every dealt commit sets it (deal)
    -- or leaves it (COALESCE with NULL).
    game_seed = CASE WHEN g.status = 'waiting' THEN NULL ELSE COALESCE(p_game_seed, game_seed) END,
    logs_packed = CASE
      WHEN p_logs_reset THEN COALESCE(p_logs_packed, '')
      WHEN g.status = 'waiting' THEN ''
      ELSE COALESCE(logs_packed, '') || COALESCE(p_logs_packed, '')
    END,
    -- round_epoch = the version at which the CURRENT round began. A new session
    -- (WAITING reset, or a GAME_START in the appended records) restarts it at 0;
    -- a round-closing move (p_closed_round) stamps it with the version this very
    -- commit produces (version + 1, since `version` here is the OLD value);
    -- every other move leaves it untouched. Same version fence as the state
    -- blob, so a client's intent version can be compared to it without a torn read.
    round_epoch = CASE
      WHEN p_logs_reset OR g.status = 'waiting' THEN 0
      WHEN p_closed_round THEN version + 1
      ELSE round_epoch
    END,
    updated_at = now(), version = version + 1
  WHERE id = p_game_id AND version = p_expected_version
  RETURNING version, round_epoch INTO v_new_version, v_round_epoch;

  IF NOT FOUND THEN
    RETURN jsonb_build_object('status', 'conflict');
  END IF;

  -- MEMBERSHIP ONLY. These tables used to carry the hands themselves; the dealt
  -- state is games.state and has been since the blob landed, so all that is
  -- left here is who is in the game. player_hands is what the realtime RLS
  -- policies EXISTS over, and the bot prune is how removing a lobby bot takes
  -- effect without a second round-trip.
  IF p_seats IS NOT NULL THEN
    INSERT INTO player_hands (game_id, player_id)
    SELECT p_game_id, s::uuid FROM jsonb_array_elements_text(p_seats) AS s
    ON CONFLICT (game_id, player_id) DO UPDATE SET updated_at = now();
  END IF;

  -- Guarded to lobby commits exactly as before: a dealt commit passes NULL (the
  -- roster cannot change mid-game), and an empty array prunes every bot because
  -- the last one was just removed.
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

  -- The session log is the packed logs_packed column (handled in the UPDATE
  -- above); per-record game_logs rows were retired in migration 20260708120000.

  -- player_views personalized-view cache (docs/PLAYER_VIEWS.md), written in THIS
  -- version-fenced transaction so it can never be torn from games.state. p_views
  -- is the CURRENT participants' masked rows; NULL means "leave the cache
  -- untouched" (a view-build failure upstream — the game still commits). An
  -- empty array means "no human participants" and correctly prunes every row.
  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, (v->>'player_id')::uuid, v->>'view', v_new_version, v->>'status', now()
    FROM jsonb_array_elements(p_views) AS v
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();

    -- Prune rows for players no longer in the game (exited / removed), so a
    -- participant who left stops seeing the game in their dashboard list.
    DELETE FROM player_views
    WHERE game_id = p_game_id
      AND player_id NOT IN (
        SELECT (v->>'player_id')::uuid FROM jsonb_array_elements(p_views) AS v
      );
  END IF;

  -- The shared spectator view (seat -1), same version fence. One row per game.
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
-- 3. create_game, without the deck insert
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION create_game(
  p_game_id   TEXT,
  p_name      TEXT,
  p_player_id UUID,
  p_players   JSONB,
  p_views     JSONB DEFAULT NULL,  -- creator's masked view cache row(s); version 0
  p_spectator TEXT  DEFAULT NULL   -- fully-masked seat -1 spectator view (bare hex); version 0
) RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  INSERT INTO games (id, name, players, status)
    VALUES (p_game_id, p_name, p_players, 'waiting');

  INSERT INTO player_hands (game_id, player_id)
    VALUES (p_game_id, p_player_id);

  -- Seed the player_views dashboard cache for the creator in the same
  -- transaction, so the new lobby is immediately readable from the client's
  -- direct player_views SELECT (docs/PLAYER_VIEWS.md). version 0 = the initial
  -- games.version.
  IF p_views IS NOT NULL THEN
    INSERT INTO player_views (game_id, player_id, view, version, status, updated_at)
    SELECT p_game_id, (v->>'player_id')::uuid, v->>'view', 0, v->>'status', now()
    FROM jsonb_array_elements(p_views) AS v
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;

  -- The shared spectator view (seat -1) for the new lobby.
  IF p_spectator IS NOT NULL THEN
    INSERT INTO spectator_views (game_id, view, version, status, updated_at)
    VALUES (p_game_id, p_spectator, 0, 'waiting', now())
    ON CONFLICT (game_id) DO UPDATE
      SET view = EXCLUDED.view, version = EXCLUDED.version,
          status = EXCLUDED.status, updated_at = now();
  END IF;
END;
$$;
