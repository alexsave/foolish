-- Local-emulation schema for the stress harness.
--
-- This stands up a REAL Postgres copy of the gameplay tables + the REAL
-- concurrency primitives the production server relies on (commit_game CAS and
-- the bot lease), lifted verbatim from supabase/migrations. RLS / realtime /
-- auth / cron are intentionally omitted: the harness connects as the service
-- role (a superuser), exactly like the edge functions do, so those layers are
-- transparent to gameplay correctness. What we keep is the part that actually
-- arbitrates concurrency: games.version + commit_game().

DROP SCHEMA IF EXISTS public CASCADE;
CREATE SCHEMA public;
DROP SCHEMA IF EXISTS auth CASCADE;
CREATE SCHEMA auth;

CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- Minimal auth.users so player_hands' FK resolves (we seed our own uuids).
CREATE TABLE auth.users (id UUID PRIMARY KEY);

CREATE TYPE player_status AS ENUM ('idle','ready','in','out');
CREATE TYPE game_status  AS ENUM ('waiting','playing','game_over');
CREATE TYPE log_type AS ENUM (
  'game_start','attack','cover','pass','pickup','good','discard',
  'defender_change','player_out','draw'
);

CREATE TABLE games (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL DEFAULT 'Untitled Game',
  deck_length INTEGER NOT NULL DEFAULT 0,
  discard_pile_length INTEGER NOT NULL DEFAULT 0,
  flipped JSONB,
  players JSONB NOT NULL DEFAULT '[]'::jsonb,
  status game_status NOT NULL DEFAULT 'waiting',
  power_suit INTEGER,
  first_attacker INTEGER,
  defender INTEGER,
  table_battles JSONB NOT NULL DEFAULT '[]'::jsonb,
  elimination_order JSONB NOT NULL DEFAULT '[]'::jsonb,
  good_timestamp BIGINT,
  good_players JSONB NOT NULL DEFAULT '[]'::jsonb,
  version BIGINT NOT NULL DEFAULT 0,
  bot_lease_token UUID,
  bot_lease_until TIMESTAMPTZ,
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

CREATE TABLE game_decks (
  game_id TEXT PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE,
  deck JSONB NOT NULL DEFAULT '[]'::jsonb,
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

CREATE TABLE player_hands (
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  player_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  hand JSONB NOT NULL DEFAULT '[]'::jsonb,
  awaiting_attack BOOLEAN NOT NULL DEFAULT false,
  joined_at TIMESTAMP DEFAULT NOW(),
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW(),
  PRIMARY KEY (game_id, player_id)
);

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

CREATE TABLE bot_hands (
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  bot_id UUID NOT NULL REFERENCES bots(id) ON DELETE CASCADE,
  hand JSONB NOT NULL DEFAULT '[]'::jsonb,
  awaiting_attack BOOLEAN NOT NULL DEFAULT false,
  joined_at TIMESTAMP DEFAULT NOW(),
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW(),
  PRIMARY KEY (game_id, bot_id)
);

CREATE TABLE game_logs (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  log_type log_type NOT NULL,
  player_id TEXT,
  card_pairs JSONB NOT NULL DEFAULT '[]'::jsonb,
  defender_index INTEGER,
  created_at TIMESTAMP DEFAULT NOW()
);

-- ============================================================================
-- The REAL concurrency primitives (verbatim from migration 20260616030000 and
-- 20260616050000). This is the actual code under test.
-- ============================================================================
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

CREATE OR REPLACE FUNCTION try_acquire_bot_lease(p_game_id TEXT, p_ttl_ms INT)
RETURNS UUID
LANGUAGE plpgsql SECURITY DEFINER SET search_path = public
AS $$
DECLARE v_token UUID;
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
LANGUAGE plpgsql SECURITY DEFINER SET search_path = public
AS $$
BEGIN
  UPDATE games SET bot_lease_until = now() - interval '1 second'
  WHERE id = p_game_id AND bot_lease_token = p_token;
END;
$$;

CREATE OR REPLACE FUNCTION renew_bot_lease(p_game_id TEXT, p_token UUID, p_ttl_ms INT)
RETURNS BOOLEAN
LANGUAGE plpgsql SECURITY DEFINER SET search_path = public
AS $$
DECLARE v_rows INT;
BEGIN
  UPDATE games SET bot_lease_until = now() + make_interval(secs => p_ttl_ms / 1000.0)
  WHERE id = p_game_id AND bot_lease_token = p_token;
  GET DIAGNOSTICS v_rows = ROW_COUNT;
  RETURN v_rows > 0;
END;
$$;
