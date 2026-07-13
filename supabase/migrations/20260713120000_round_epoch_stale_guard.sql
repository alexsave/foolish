-- Round-boundary guard for stale-intent actions (docs/WEB_RACE_BUG_HANDOFF.md).
--
-- The server serializes every move through the version-fenced commit_game CAS,
-- but serialization is not intent-preservation: a move that was in flight while
-- a defender's pickup (or a discard) CLOSED the round can re-validate against
-- the fresh round and apply as a brand-new attack — the "revert, then it plays
-- anyway" ghost the owner reported. The fix is a round-scoped (not
-- version-scoped) guard: track the version at which the current round began and
-- reject any move the client composed against an earlier round.
--
--   games.round_epoch = the `version` at which the CURRENT round began.
--
-- Bumped to the new version on any commit whose move closed a round; reset to 0
-- on a new session (WAITING reset / GAME_START). The action edge function
-- rejects a move whose client-intent version < round_epoch with
-- REJECT_STALE_ROUND. 0 (the default) means "round 1 / never closed" — the
-- guard is a no-op, so existing in-flight games need no backfill.

ALTER TABLE games ADD COLUMN IF NOT EXISTS round_epoch BIGINT NOT NULL DEFAULT 0;

-- Signature change: the new p_closed_round param means PostgREST must not see
-- two overloads. Drop the 12-arg version (migration 20260709120000) first.
DROP FUNCTION IF EXISTS commit_game(
  TEXT, BIGINT, JSONB, JSONB, JSONB, JSONB, TEXT, TEXT, BOOLEAN, TEXT, JSONB, TEXT
);

CREATE OR REPLACE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_deck             JSONB,
  p_hands            JSONB,
  p_bot_hands        JSONB,
  p_state            TEXT    DEFAULT NULL,  -- packed kernel state blob (hex); NULL leaves the column unchanged (never-dealt games)
  p_logs_packed      TEXT    DEFAULT NULL,  -- this move's logwire records (bare hex), appended under the version fence
  p_logs_reset       BOOLEAN DEFAULT FALSE, -- session reset (GAME_START in the records): replace instead of append
  p_game_seed        TEXT    DEFAULT NULL,  -- deal seed (hex); set once at the deal, NULL on every other commit leaves it unchanged
  p_views            JSONB   DEFAULT NULL,  -- per-participant masked view cache rows [{player_id,view,status}]; NULL leaves player_views untouched
  p_spectator        TEXT    DEFAULT NULL,  -- fully-masked seat -1 spectator view (bare hex); NULL leaves spectator_views untouched
  p_closed_round     BOOLEAN DEFAULT FALSE  -- TRUE when THIS move closed a round (pickup/discard): stamp round_epoch with the new version
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
    state = CASE WHEN g.status = 'waiting' THEN NULL ELSE COALESCE(p_state, state) END,
    game_seed = CASE WHEN g.status = 'waiting' THEN NULL ELSE COALESCE(p_game_seed, game_seed) END,
    logs_packed = CASE
      WHEN p_logs_reset THEN COALESCE(p_logs_packed, '')
      WHEN g.status = 'waiting' THEN ''
      ELSE COALESCE(logs_packed, '') || COALESCE(p_logs_packed, '')
    END,
    -- round_epoch: 0 on a new session (reset/waiting); the version this commit
    -- produces on a round-closing move (version + 1, since `version` is the OLD
    -- value here); unchanged otherwise. Same version fence as the state blob.
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

  IF p_deck IS NOT NULL THEN
    INSERT INTO game_decks (game_id, deck) VALUES (p_game_id, p_deck)
    ON CONFLICT (game_id) DO UPDATE SET deck = EXCLUDED.deck, updated_at = now();
  END IF;

  IF p_hands IS NOT NULL AND jsonb_array_length(p_hands) > 0 THEN
    INSERT INTO player_hands (game_id, player_id, hand, awaiting_attack)
    SELECT p_game_id, (h->>'player_id')::uuid, h->'hand',
           COALESCE((h->>'awaiting_attack')::bool, false)
    FROM jsonb_array_elements(p_hands) AS h
    ON CONFLICT (game_id, player_id) DO UPDATE
      SET hand = EXCLUDED.hand, awaiting_attack = EXCLUDED.awaiting_attack, updated_at = now();
  END IF;

  IF p_bot_hands IS NOT NULL THEN
    IF jsonb_array_length(p_bot_hands) > 0 THEN
      INSERT INTO bot_hands (game_id, bot_id, hand, awaiting_attack)
      SELECT p_game_id, (b->>'bot_id')::uuid, b->'hand',
             COALESCE((b->>'awaiting_attack')::bool, false)
      FROM jsonb_array_elements(p_bot_hands) AS b
      ON CONFLICT (game_id, bot_id) DO UPDATE
        SET hand = EXCLUDED.hand, awaiting_attack = EXCLUDED.awaiting_attack, updated_at = now();
    END IF;

    DELETE FROM bot_hands
    WHERE game_id = p_game_id
      AND bot_id NOT IN (
        SELECT (b->>'bot_id')::uuid FROM jsonb_array_elements(p_bot_hands) AS b
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
