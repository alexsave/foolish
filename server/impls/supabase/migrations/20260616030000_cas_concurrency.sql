-- Replace the two table-row "locks" (game_locks, bot_locks) with correct
-- primitives that actually work on Supabase (Postgres + PostgREST + ephemeral
-- isolates, no held transactions across the TS compute):
--
--   1. games.version  — optimistic-concurrency token. Every state write goes
--      through commit_game(), which only succeeds if the stored version still
--      equals the one the caller loaded, then bumps it. This is the fencing that
--      makes double-execution (the duplicate-card bug) impossible, and it does
--      the whole multi-table write in ONE transaction (no torn reads).
--
--   2. games.bot_lease_* — an atomic, auto-expiring lease replacing the bot_locks
--      baton: ensures one bot-driver loop per game without a leak-prone
--      finally-release. Claimed/renewed/released by single conditional UPDATEs.
--
-- Idempotent; safe to re-run.

-- 1. Columns ------------------------------------------------------------------
ALTER TABLE games
  ADD COLUMN IF NOT EXISTS version         BIGINT NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS bot_lease_token UUID,
  ADD COLUMN IF NOT EXISTS bot_lease_until TIMESTAMPTZ;

-- 2. Atomic versioned commit --------------------------------------------------
-- Writes games + game_decks + player_hands + bot_hands in one transaction, gated
-- on the expected version. Returns {status:'ok',version:N} or {status:'conflict'}.
-- Logs are intentionally NOT here: they are append-only and keyed by UUID
-- (idempotent), so a torn log write can't corrupt game state — they stay a
-- separate best-effort upsert.
CREATE OR REPLACE FUNCTION commit_game(
  p_game_id          TEXT,
  p_expected_version BIGINT,
  p_game             JSONB,
  p_deck             JSONB,
  p_hands            JSONB,
  p_bot_hands        JSONB
) RETURNS JSONB
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_new_version BIGINT;
  g games%ROWTYPE;
BEGIN
  -- Shovel the public-game JSON (built in TS by gameToPublicGame) onto a games
  -- row; jsonb_populate_record auto-casts every field (enum/int/jsonb). We then
  -- write only the gameplay columns, preserving version/lease/created_at, and bump
  -- the version. No game logic here — just an atomic, version-gated write.
  g := jsonb_populate_record(NULL::games, p_game);

  UPDATE games SET
    name = g.name, deck_length = g.deck_length, discard_pile_length = g.discard_pile_length,
    flipped = g.flipped, players = g.players, status = g.status, power_suit = g.power_suit,
    first_attacker = g.first_attacker, defender = g.defender, table_battles = g.table_battles,
    elimination_order = g.elimination_order, good_timestamp = g.good_timestamp,
    good_players = g.good_players, updated_at = now(), version = version + 1
  WHERE id = p_game_id AND version = p_expected_version
  RETURNING version INTO v_new_version;

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

  IF p_bot_hands IS NOT NULL AND jsonb_array_length(p_bot_hands) > 0 THEN
    INSERT INTO bot_hands (game_id, bot_id, hand, awaiting_attack)
    SELECT p_game_id, (b->>'bot_id')::uuid, b->'hand',
           COALESCE((b->>'awaiting_attack')::bool, false)
    FROM jsonb_array_elements(p_bot_hands) AS b
    ON CONFLICT (game_id, bot_id) DO UPDATE
      SET hand = EXCLUDED.hand, awaiting_attack = EXCLUDED.awaiting_attack, updated_at = now();
  END IF;

  RETURN jsonb_build_object('status', 'ok', 'version', v_new_version);
END;
$$;

-- 3. Bot-loop lease (replaces bot_locks) --------------------------------------
-- Single atomic claim: succeeds only if no live lease exists. Returns the new
-- token, or NULL if another loop holds it. Auto-expires (no finally needed).
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

-- Best-effort early release (fenced on our token). If we never call it, the
-- lease just expires on its own.
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

-- 4. The obsolete lock tables are dropped SEPARATELY, AFTER the new functions are
-- deployed — see migrations/20260616030001_drop_lock_tables.sql. Dropping them
-- here would break the still-live OLD functions (which insert into them) during
-- the window between this migration and the function deploy. Until then the empty
-- tables just sit there harmlessly.
