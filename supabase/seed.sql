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
DROP TABLE IF EXISTS private_user_channel CASCADE;
DROP TABLE IF EXISTS public_game_channel CASCADE;
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

-- Games table - stores complete game state as JSONB
CREATE TABLE games (
  id TEXT PRIMARY KEY,
  deck JSONB NOT NULL DEFAULT '[]'::jsonb, -- Card[]
  flipped JSONB, -- Card | null
  players JSONB NOT NULL DEFAULT '[]'::jsonb, -- Player[]
  status game_status NOT NULL DEFAULT 'waiting',
  power_suit INTEGER,
  first_attacker INTEGER,
  currently_attacked INTEGER,
  previous_first_attacker INTEGER,
  previous_currently_attacked INTEGER,
  table_battles JSONB NOT NULL DEFAULT '[]'::jsonb, -- Battle[]
  created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
  updated_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

-- Junction table for player-game relationships
CREATE TABLE player_games (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  player_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  joined_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
  UNIQUE(player_id, game_id)
);

-- Public game channel for real-time game messages
CREATE TABLE public_game_channel (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  message JSONB NOT NULL, -- Message object
  created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

-- Private user channel for direct messages
CREATE TABLE private_user_channel (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  user_id UUID NOT NULL REFERENCES auth.users(id) ON DELETE CASCADE,
  message JSONB NOT NULL, -- Message object
  created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
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

-- Create indexes for better performance
CREATE INDEX idx_games_status ON games(status);
CREATE INDEX idx_player_games_player_id ON player_games(player_id);
CREATE INDEX idx_player_games_game_id ON player_games(game_id);
CREATE INDEX idx_public_game_channel_game_id ON public_game_channel(game_id);
CREATE INDEX idx_public_game_channel_created_at ON public_game_channel(created_at);
CREATE INDEX idx_private_user_channel_user_id ON private_user_channel(user_id);
CREATE INDEX idx_private_user_channel_created_at ON private_user_channel(created_at);
CREATE INDEX idx_chat_messages_game_id ON chat_messages(game_id);
CREATE INDEX idx_chat_messages_user_id ON chat_messages(user_id);
CREATE INDEX idx_chat_messages_created_at ON chat_messages(created_at);

-- Enable Row Level Security
ALTER TABLE games ENABLE ROW LEVEL SECURITY;
ALTER TABLE player_games ENABLE ROW LEVEL SECURITY;
ALTER TABLE public_game_channel ENABLE ROW LEVEL SECURITY;
ALTER TABLE private_user_channel ENABLE ROW LEVEL SECURITY;
ALTER TABLE chat_messages ENABLE ROW LEVEL SECURITY;

-- RLS Policies

-- Games: players can view games they're in, anyone can create
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

-- Player-games: players can view their own relationships
CREATE POLICY "Users can view their game memberships" ON player_games
  FOR SELECT USING (player_id = auth.uid());

CREATE POLICY "Users can join games" ON player_games
  FOR INSERT WITH CHECK (player_id = auth.uid());

CREATE POLICY "Users can leave games" ON player_games
  FOR DELETE USING (player_id = auth.uid());

-- Public game channel: players can view messages for games they're in
CREATE POLICY "Players can view game messages" ON public_game_channel
  FOR SELECT USING (
    game_id IN (
      SELECT game_id FROM player_games 
      WHERE player_id = auth.uid()
    )
  );

CREATE POLICY "Players can send game messages" ON public_game_channel
  FOR INSERT WITH CHECK (
    game_id IN (
      SELECT game_id FROM player_games 
      WHERE player_id = auth.uid()
    )
  );

-- Private user channel: users can only see their own messages
CREATE POLICY "Users can view own private messages" ON private_user_channel
  FOR SELECT USING (user_id = auth.uid());

CREATE POLICY "Users can receive private messages" ON private_user_channel
  FOR INSERT WITH CHECK (user_id = auth.uid());

-- Chat messages: players can view messages for games they're in
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

-- Functions for automatic updated_at timestamps
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- Helper function to get user's games
CREATE OR REPLACE FUNCTION get_user_games(user_id_param UUID)
RETURNS TABLE(game_id TEXT, game_data JSONB) AS $$
BEGIN
  RETURN QUERY
  SELECT g.id as game_id, to_jsonb(g.*) as game_data
  FROM games g
  INNER JOIN player_games pg ON g.id = pg.game_id
  WHERE pg.player_id = user_id_param;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- =============================================================================
-- TRIGGERS: Set up automatic triggers
-- =============================================================================

CREATE TRIGGER update_games_updated_at 
  BEFORE UPDATE ON games
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
DROP POLICY IF EXISTS "service role can send game broadcasts" ON "realtime"."messages";
DROP POLICY IF EXISTS "service role can send private messages" ON "realtime"."messages";

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
-- TEMPORARY: Allow any authenticated user to read from user channels for testing
CREATE POLICY "authenticated can receive private messages"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) = CONCAT('user-', split_part((current_setting('request.jwt.claims', true)::jsonb ->> 'email'), '@', 1))
  AND realtime.messages.extension IN ('broadcast')
);

-- Anyone can send private messages to any user (for system notifications)
-- In production, you might want to restrict this further
CREATE POLICY "authenticated can send private messages"
ON "realtime"."messages"
FOR INSERT
TO authenticated
WITH CHECK (
  (SELECT realtime.topic()) LIKE 'user-%'
  AND realtime.messages.extension IN ('broadcast')
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

-- =============================================================================
-- SETUP COMPLETE!
-- Your database schema is now ready for the game application with Realtime Authorization.
-- =============================================================================
