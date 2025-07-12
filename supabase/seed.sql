-- =============================================================================
-- Supabase Schema for Game Application
-- Copy and paste this entire script into Supabase's SQL Editor
-- =============================================================================

-- Enable necessary extensions first
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- =============================================================================
-- CLEANUP: Drop existing objects in correct dependency order
-- =============================================================================

-- Drop functions first (triggers will be dropped automatically with tables)
DROP FUNCTION IF EXISTS update_updated_at_column() CASCADE;

-- Drop tables in reverse dependency order (this will automatically drop all policies and triggers)
DROP TABLE IF EXISTS chat_messages CASCADE;
DROP TABLE IF EXISTS player_hands CASCADE;
DROP TABLE IF EXISTS game_decks CASCADE;
DROP TABLE IF EXISTS games CASCADE;

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
  'first_attacker', 
  'free_play',
  'only_defend',
  'wait_for_attackers'
);

-- =============================================================================
-- MAIN TABLES: Separated for security
-- =============================================================================

-- Games table - stores PUBLIC game state only (no deck, no hands)
CREATE TABLE games (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL DEFAULT 'Untitled Game',
  deck_length INTEGER NOT NULL DEFAULT 0,
  flipped JSONB, -- Card | null - public info
  players JSONB NOT NULL DEFAULT '[]'::jsonb, -- Player[] WITHOUT hands - only name, id, status, position, hand_length
  status game_status NOT NULL DEFAULT 'waiting',
  power_suit INTEGER,
  first_attacker INTEGER,
  defender INTEGER,
  table_battles JSONB NOT NULL DEFAULT '[]'::jsonb, -- Battle[] - public info
  elimination_order JSONB NOT NULL DEFAULT '[]'::jsonb, -- Array of player_ids in order they were eliminated
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

-- Game decks table - SENSITIVE: Only edge functions can access
CREATE TABLE game_decks (
  game_id TEXT PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE,
  deck JSONB NOT NULL DEFAULT '[]'::jsonb, -- Card[] - deck cards
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

-- Player hands table - SENSITIVE: Players can only see their own hands
-- This table also serves as the player-game relationship table
CREATE TABLE player_hands (
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  player_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  hand JSONB NOT NULL DEFAULT '[]'::jsonb, -- Card[] - player's cards
  awaiting_attack BOOLEAN NOT NULL DEFAULT false, -- Private status for attack confirmation
  joined_at TIMESTAMP DEFAULT NOW(),
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW(),
  PRIMARY KEY (game_id, player_id) -- One hand per player per game
);

-- Chat messages table
CREATE TABLE chat_messages (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  message TEXT NOT NULL,
  is_system BOOLEAN NOT NULL DEFAULT false,
  created_at TIMESTAMP DEFAULT NOW()
);

-- User ELO ratings table
CREATE TABLE user_elo_ratings (
  user_id UUID PRIMARY KEY REFERENCES auth.users(id) ON DELETE CASCADE,
  elo_rating INTEGER NOT NULL DEFAULT 1000,
  games_played INTEGER NOT NULL DEFAULT 0,
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

-- =============================================================================
-- INDEXES: Create indexes for better performance
-- =============================================================================

CREATE INDEX idx_games_status ON games(status);
CREATE INDEX idx_games_name ON games(name);
CREATE INDEX idx_game_decks_game_id ON game_decks(game_id);
CREATE INDEX idx_player_hands_game_id ON player_hands(game_id);
CREATE INDEX idx_player_hands_player_id ON player_hands(player_id);
CREATE INDEX idx_chat_messages_game_id ON chat_messages(game_id);
CREATE INDEX idx_chat_messages_user_id ON chat_messages(user_id);
CREATE INDEX idx_chat_messages_created_at ON chat_messages(created_at);
CREATE INDEX idx_games_updated_at ON games(updated_at);
CREATE INDEX idx_user_elo_ratings_user_id ON user_elo_ratings(user_id);
CREATE INDEX idx_user_elo_ratings_elo_rating ON user_elo_ratings(elo_rating);

-- =============================================================================
-- ROW LEVEL SECURITY: Enable RLS on all tables
-- =============================================================================

ALTER TABLE games ENABLE ROW LEVEL SECURITY;
ALTER TABLE game_decks ENABLE ROW LEVEL SECURITY;
ALTER TABLE player_hands ENABLE ROW LEVEL SECURITY;
ALTER TABLE chat_messages ENABLE ROW LEVEL SECURITY;
ALTER TABLE user_elo_ratings ENABLE ROW LEVEL SECURITY;

-- =============================================================================
-- RLS POLICIES: Security-first approach
-- =============================================================================

-- Games: Anyone can view games (PUBLIC DATA ONLY - no sensitive info)
-- This allows users to join games or spectate without being in the game first
CREATE POLICY "Anyone can view games" ON games
  FOR SELECT USING (true);

CREATE POLICY "Anyone can create games" ON games
  FOR INSERT WITH CHECK (true);

CREATE POLICY "Players can update games they're in" ON games
  FOR UPDATE USING (
    id IN (
      SELECT game_id FROM player_hands 
      WHERE player_id = auth.uid()
    )
  );

-- Game Decks: ONLY service role can access (edge functions only)
CREATE POLICY "Only service role can access game decks" ON game_decks
  FOR ALL USING (auth.role() = 'service_role');

-- Player Hands: Players can ONLY see their own hands
CREATE POLICY "Players can view own hand only" ON player_hands
  FOR SELECT USING (player_id = auth.uid());

CREATE POLICY "Players can join games" ON player_hands
  FOR INSERT WITH CHECK (player_id = auth.uid());

CREATE POLICY "Players can leave games" ON player_hands
  FOR DELETE USING (player_id = auth.uid());

CREATE POLICY "Service role can manage all hands" ON player_hands
  FOR ALL USING (auth.role() = 'service_role');

-- Chat messages: Players can view messages for games they're in
CREATE POLICY "Players can view chat messages for their games" ON chat_messages
  FOR SELECT USING (
    game_id IN (
      SELECT game_id FROM player_hands 
      WHERE player_id = auth.uid()
    )
  );

CREATE POLICY "Players can send chat messages to their games" ON chat_messages
  FOR INSERT WITH CHECK (
    game_id IN (
      SELECT game_id FROM player_hands 
      WHERE player_id = auth.uid()
    ) AND user_id = auth.uid()
  );

-- User ELO ratings: Read-only for authenticated users
CREATE POLICY "Anyone can view ELO ratings" ON user_elo_ratings
  FOR SELECT USING (true);

CREATE POLICY "Only service role can manage ELO ratings" ON user_elo_ratings
  FOR ALL USING (auth.role() = 'service_role');

-- =============================================================================
-- FUNCTIONS: Helper functions and triggers
-- =============================================================================

-- Functions for automatic updated_at timestamps
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- Function to broadcast chat message changes
CREATE OR REPLACE FUNCTION public.chat_messages_changes()
RETURNS TRIGGER
SECURITY DEFINER
LANGUAGE plpgsql
AS $$
BEGIN
  -- Broadcast to game-specific topic for chat messages
  PERFORM realtime.broadcast_changes(
    'chat:' || COALESCE(NEW.game_id, OLD.game_id)::text, -- topic - chat:{game_id}
    TG_OP,                                                -- event - INSERT, UPDATE, DELETE
    TG_OP,                                                -- operation - same as event
    TG_TABLE_NAME,                                        -- table - chat_messages
    TG_TABLE_SCHEMA,                                      -- schema - public
    NEW,                                                  -- new record - the record after the change
    OLD                                                   -- old record - the record before the change
  );
  RETURN COALESCE(NEW, OLD);
END;
$$;

-- Function to create default ELO rating for new users
CREATE OR REPLACE FUNCTION public.create_default_elo_rating()
RETURNS TRIGGER
SECURITY DEFINER
LANGUAGE plpgsql
AS $$
BEGIN
  INSERT INTO public.user_elo_ratings (user_id, elo_rating, games_played)
  VALUES (NEW.id, 1000, 0);
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

CREATE TRIGGER update_game_decks_updated_at 
  BEFORE UPDATE ON game_decks
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_player_hands_updated_at 
  BEFORE UPDATE ON player_hands
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_user_elo_ratings_updated_at 
  BEFORE UPDATE ON user_elo_ratings
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

-- Trigger for chat message changes
CREATE TRIGGER handle_chat_messages_changes
  AFTER INSERT OR UPDATE OR DELETE
  ON public.chat_messages
  FOR EACH ROW
  EXECUTE FUNCTION chat_messages_changes();

-- Trigger to create default ELO rating for new users
CREATE TRIGGER handle_new_user_elo_rating
  AFTER INSERT
  ON auth.users
  FOR EACH ROW
  EXECUTE FUNCTION create_default_elo_rating();

-- =============================================================================
-- REALTIME AUTHORIZATION POLICIES
-- Enable Supabase Realtime with proper security
-- =============================================================================

-- Enable RLS on the realtime messages table
ALTER TABLE IF EXISTS realtime.messages ENABLE ROW LEVEL SECURITY;

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


-- Policy for private user channels (topic: user-{email_prefix})
-- Users can read messages sent to them  
CREATE POLICY "authenticated can receive private messages"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) = CONCAT('user-', split_part((current_setting('request.jwt.claims', true)::jsonb ->> 'email'), '@', 1))
  AND realtime.messages.extension IN ('broadcast')
);

-- Anyone can send private messages to any user (for system notifications)
CREATE POLICY "authenticated can send private messages"
ON "realtime"."messages"
FOR INSERT
TO authenticated
WITH CHECK (
  (SELECT realtime.topic()) LIKE 'user-%'
  AND realtime.messages.extension IN ('broadcast')
);

-- Policy for private game-user channels (topic: gu-{game_id}-{user_id})
-- Users can only read messages from their own game-user channels
CREATE POLICY "authenticated can receive game-user messages"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'gu-%' AND
  -- Extract user_id from topic (gu-{game_id}-{user_id}) and verify it matches current user
  split_part((SELECT realtime.topic()), '-', 3) = auth.uid()::text AND
  -- Extract game_id and verify user is in that game
  EXISTS (
    SELECT 1
    FROM player_hands
    WHERE 
      player_id = auth.uid()
      AND game_id = split_part((SELECT realtime.topic()), '-', 2)
  ) AND
  realtime.messages.extension IN ('broadcast')
);

-- Policy for chat broadcasts (topic: chat-{game_id})
-- Users can receive chat broadcasts for games they're participating in
CREATE POLICY "authenticated can receive chat broadcasts"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'chat:%' AND
  -- Extract game_id from topic (chat:{game_id}) and verify user is in that game
  EXISTS (
    SELECT 1
    FROM player_hands
    WHERE 
      player_id = auth.uid()
      AND game_id = split_part((SELECT realtime.topic()), ':', 2)
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

-- Service role can send private messages (for server notifications)
CREATE POLICY "service role can send private messages"
ON "realtime"."messages"
FOR INSERT
TO service_role  
WITH CHECK (
  (SELECT realtime.topic()) LIKE 'user-%'
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
-- POSTGRESQL ADVISORY LOCK FUNCTIONS
-- For game operation synchronization across function instances
-- =============================================================================

-- Function to try acquiring an advisory lock (non-blocking)
CREATE OR REPLACE FUNCTION pg_try_advisory_lock(key bigint)
RETURNS boolean
LANGUAGE sql
SECURITY DEFINER
AS $$
  SELECT pg_try_advisory_lock($1);
$$;

-- Function to release an advisory lock
CREATE OR REPLACE FUNCTION pg_advisory_unlock(key bigint)
RETURNS boolean
LANGUAGE sql
SECURITY DEFINER
AS $$
  SELECT pg_advisory_unlock($1);
$$;

-- Function to try acquiring an advisory lock with string key (convenience function)
CREATE OR REPLACE FUNCTION pg_try_advisory_lock_string(key text)
RETURNS boolean
LANGUAGE sql
SECURITY DEFINER
AS $$
  SELECT pg_try_advisory_lock(hashtext($1));
$$;

-- Function to release an advisory lock with string key (convenience function)
CREATE OR REPLACE FUNCTION pg_advisory_unlock_string(key text)
RETURNS boolean
LANGUAGE sql
SECURITY DEFINER
AS $$
  SELECT pg_advisory_unlock(hashtext($1));
$$;

-- Grant execute permissions to authenticated users
GRANT EXECUTE ON FUNCTION pg_try_advisory_lock(bigint) TO authenticated;
GRANT EXECUTE ON FUNCTION pg_advisory_unlock(bigint) TO authenticated;
GRANT EXECUTE ON FUNCTION pg_try_advisory_lock_string(text) TO authenticated;
GRANT EXECUTE ON FUNCTION pg_advisory_unlock_string(text) TO authenticated;

-- Also grant to service_role for function usage
GRANT EXECUTE ON FUNCTION pg_try_advisory_lock(bigint) TO service_role;
GRANT EXECUTE ON FUNCTION pg_advisory_unlock(bigint) TO service_role;
GRANT EXECUTE ON FUNCTION pg_try_advisory_lock_string(text) TO service_role;
GRANT EXECUTE ON FUNCTION pg_advisory_unlock_string(text) TO service_role;

-- =============================================================================
-- SETUP COMPLETE!
-- Your database schema is now secure and ready for the game application.
-- Advisory locks are configured for game operation synchronization.
-- =============================================================================
