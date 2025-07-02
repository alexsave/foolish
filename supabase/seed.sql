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
DROP FUNCTION IF EXISTS get_user_games(UUID) CASCADE;

-- Drop tables in reverse dependency order (this will automatically drop all policies and triggers)
DROP TABLE IF EXISTS chat_messages CASCADE;
DROP TABLE IF EXISTS player_hands CASCADE;
DROP TABLE IF EXISTS game_decks CASCADE;
DROP TABLE IF EXISTS player_games CASCADE;
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
  'out',
  'awaiting_attack'
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
  currently_attacked INTEGER,
  table_battles JSONB NOT NULL DEFAULT '[]'::jsonb, -- Battle[] - public info
  created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
  updated_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

-- Game decks table - SENSITIVE: Only edge functions can access
CREATE TABLE game_decks (
  game_id TEXT PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE,
  deck JSONB NOT NULL DEFAULT '[]'::jsonb, -- Card[] - deck cards
  created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
  updated_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

-- Player hands table - SENSITIVE: Players can only see their own hands
CREATE TABLE player_hands (
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  player_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  hand JSONB NOT NULL DEFAULT '[]'::jsonb, -- Card[] - player's cards
  created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
  updated_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
  PRIMARY KEY (game_id, player_id) -- One hand per player per game
);

-- Junction table for player-game relationships
CREATE TABLE player_games (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  player_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  joined_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
  UNIQUE(player_id, game_id)
);

-- Chat messages table
CREATE TABLE chat_messages (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  message TEXT NOT NULL,
  is_system BOOLEAN NOT NULL DEFAULT false,
  created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

-- =============================================================================
-- INDEXES: Create indexes for better performance
-- =============================================================================

CREATE INDEX idx_games_status ON games(status);
CREATE INDEX idx_games_name ON games(name);
CREATE INDEX idx_game_decks_game_id ON game_decks(game_id);
CREATE INDEX idx_player_hands_game_id ON player_hands(game_id);
CREATE INDEX idx_player_hands_player_id ON player_hands(player_id);
CREATE INDEX idx_player_games_player_id ON player_games(player_id);
CREATE INDEX idx_player_games_game_id ON player_games(game_id);
CREATE INDEX idx_chat_messages_game_id ON chat_messages(game_id);
CREATE INDEX idx_chat_messages_user_id ON chat_messages(user_id);
CREATE INDEX idx_chat_messages_created_at ON chat_messages(created_at);

-- =============================================================================
-- ROW LEVEL SECURITY: Enable RLS on all tables
-- =============================================================================

ALTER TABLE games ENABLE ROW LEVEL SECURITY;
ALTER TABLE game_decks ENABLE ROW LEVEL SECURITY;
ALTER TABLE player_hands ENABLE ROW LEVEL SECURITY;
ALTER TABLE player_games ENABLE ROW LEVEL SECURITY;
ALTER TABLE chat_messages ENABLE ROW LEVEL SECURITY;

-- =============================================================================
-- RLS POLICIES: Security-first approach
-- =============================================================================

-- Games: Players can view games they're in (PUBLIC DATA ONLY - no sensitive info)
CREATE POLICY "Users can view games they're in" ON games
  FOR SELECT USING (
    id IN (
      SELECT game_id FROM player_games 
      WHERE player_id = auth.uid()
    )
  );

CREATE POLICY "Anyone can create games" ON games
  FOR INSERT WITH CHECK (true);

CREATE POLICY "Players can update games they're in" ON games
  FOR UPDATE USING (
    id IN (
      SELECT game_id FROM player_games 
      WHERE player_id = auth.uid()
    )
  );

-- Game Decks: ONLY service role can access (edge functions only)
CREATE POLICY "Only service role can access game decks" ON game_decks
  FOR ALL USING (auth.role() = 'service_role');

-- Player Hands: Players can ONLY see their own hands
CREATE POLICY "Players can view own hand only" ON player_hands
  FOR SELECT USING (player_id = auth.uid());

CREATE POLICY "Service role can manage all hands" ON player_hands
  FOR ALL USING (auth.role() = 'service_role');

-- Player-games: Players can view their own relationships
CREATE POLICY "Users can view their game memberships" ON player_games
  FOR SELECT USING (player_id = auth.uid());

CREATE POLICY "Users can join games" ON player_games
  FOR INSERT WITH CHECK (player_id = auth.uid());

CREATE POLICY "Users can leave games" ON player_games
  FOR DELETE USING (player_id = auth.uid());

-- Chat messages: Players can view messages for games they're in
CREATE POLICY "Players can view chat messages for their games" ON chat_messages
  FOR SELECT USING (
    game_id IN (
      SELECT game_id FROM player_games 
      WHERE player_id = auth.uid()
    )
  );

CREATE POLICY "Players can send chat messages to their games" ON chat_messages
  FOR INSERT WITH CHECK (
    game_id IN (
      SELECT game_id FROM player_games 
      WHERE player_id = auth.uid()
    ) AND user_id = auth.uid()
  );

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

-- Helper function to get user's games with basic info (no sensitive data)
CREATE OR REPLACE FUNCTION get_user_games(user_id_param UUID)
RETURNS TABLE(
  game_id TEXT, 
  game_name TEXT,
  game_status game_status,
  player_count INTEGER,
  deck_length INTEGER,
  created_at TIMESTAMP WITH TIME ZONE
) AS $$
BEGIN
  RETURN QUERY
  SELECT 
    g.id as game_id, 
    g.name as game_name,
    g.status as game_status,
    g.deck_length as deck_length,
    jsonb_array_length(g.players) as player_count,
    g.created_at
  FROM games g
  INNER JOIN player_games pg ON g.id = pg.game_id
  WHERE pg.player_id = user_id_param
  ORDER BY g.created_at DESC;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- Helper function to get player's hand for a specific game
CREATE OR REPLACE FUNCTION get_player_hand(game_id_param TEXT, player_id_param UUID)
RETURNS JSONB AS $$
DECLARE
  player_hand JSONB;
BEGIN
  SELECT hand INTO player_hand
  FROM player_hands
  WHERE game_id = game_id_param AND player_id = player_id_param;
  
  RETURN COALESCE(player_hand, '[]'::jsonb);
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

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

-- =============================================================================
-- REALTIME AUTHORIZATION POLICIES
-- Enable Supabase Realtime with proper security
-- =============================================================================

-- Enable RLS on the realtime messages table
ALTER TABLE IF EXISTS realtime.messages ENABLE ROW LEVEL SECURITY;

-- Drop existing policies if they exist
DROP POLICY IF EXISTS "authenticated can receive game broadcasts" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can send game broadcasts" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can receive private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can send private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can receive game-user messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can send game-user messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "service role can send game broadcasts" ON "realtime"."messages";
DROP POLICY IF EXISTS "service role can send private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "service role can send game-user messages" ON "realtime"."messages";

-- Policy for public game channels (topic: game-{game_id})
-- Players can read messages from games they're in
CREATE POLICY "authenticated can receive game broadcasts"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  EXISTS (
    SELECT 1
    FROM player_games
    WHERE 
      player_id = auth.uid()
      AND game_id = REPLACE((SELECT realtime.topic()), 'game-', '')
      AND realtime.messages.extension IN ('broadcast')
  )
);

CREATE POLICY "authenticated can send game broadcasts"
ON "realtime"."messages"
FOR INSERT
TO authenticated
WITH CHECK (
  EXISTS (
    SELECT 1
    FROM player_games
    WHERE 
      player_id = auth.uid()
      AND game_id = REPLACE((SELECT realtime.topic()), 'game-', '')
      AND realtime.messages.extension IN ('broadcast')
  )
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

-- Policy for private game-user channels (topic: gu-{game_id}-{username})
-- Users can only read messages from their own game-user channels
CREATE POLICY "authenticated can receive game-user messages"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'gu-%' AND
  -- Extract username from topic (gu-{game_id}-{username}) and verify it matches current user
  split_part((SELECT realtime.topic()), '-', 3) = split_part((current_setting('request.jwt.claims', true)::jsonb ->> 'email'), '@', 1) AND
  -- Extract game_id and verify user is in that game
  EXISTS (
    SELECT 1
    FROM player_games
    WHERE 
      player_id = auth.uid()
      AND game_id = split_part((SELECT realtime.topic()), '-', 2)
  ) AND
  realtime.messages.extension IN ('broadcast')
);

CREATE POLICY "authenticated can send game-user messages"
ON "realtime"."messages"
FOR INSERT
TO authenticated
WITH CHECK (
  (SELECT realtime.topic()) LIKE 'gu-%' AND
  -- Extract username from topic and verify it matches current user
  split_part((SELECT realtime.topic()), '-', 3) = split_part((current_setting('request.jwt.claims', true)::jsonb ->> 'email'), '@', 1) AND
  -- Extract game_id and verify user is in that game
  EXISTS (
    SELECT 1
    FROM player_games
    WHERE 
      player_id = auth.uid()
      AND game_id = split_part((SELECT realtime.topic()), '-', 2)
  ) AND
  realtime.messages.extension IN ('broadcast')
);

-- =============================================================================
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
-- SETUP COMPLETE!
-- Your database schema is now secure and ready for the game application.
-- =============================================================================
