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
DROP TABLE IF EXISTS bot_hands CASCADE;
DROP TABLE IF EXISTS player_hands CASCADE;
DROP TABLE IF EXISTS game_decks CASCADE;
DROP TABLE IF EXISTS games CASCADE;
DROP TABLE IF EXISTS user_elo_ratings CASCADE;
DROP TABLE IF EXISTS bots CASCADE;

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
  'game_over'
);

-- =============================================================================
-- MAIN TABLES: Separated for security
-- =============================================================================

-- Games table - stores PUBLIC game state only (no deck, no hands)
CREATE TABLE games (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL DEFAULT 'Untitled Game',
  deck_length INTEGER NOT NULL DEFAULT 0,
  discard_pile_length INTEGER NOT NULL DEFAULT 0,
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
  previous_elo INTEGER NOT NULL DEFAULT 1000,
  games_played INTEGER NOT NULL DEFAULT 0,
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW()
);

-- Bots table - AI players with strategies
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

-- Bot hands table - SENSITIVE: Only edge functions can access (similar to player_hands)
CREATE TABLE bot_hands (
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  bot_id UUID NOT NULL REFERENCES bots(id) ON DELETE CASCADE,
  hand JSONB NOT NULL DEFAULT '[]'::jsonb, -- Card[] - bot's cards
  awaiting_attack BOOLEAN NOT NULL DEFAULT false, -- Private status for attack confirmation
  done_attacking_this_round BOOLEAN NOT NULL DEFAULT false, -- Flag to indicate bot is done attacking this round
  joined_at TIMESTAMP DEFAULT NOW(),
  created_at TIMESTAMP DEFAULT NOW(),
  updated_at TIMESTAMP DEFAULT NOW(),
  PRIMARY KEY (game_id, bot_id) -- One hand per bot per game
);

-- Bot locks table - Simple table-based locking for bot processing
CREATE TABLE bot_locks (
  game_id TEXT PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE,
  lock_id TEXT NOT NULL, -- Random ID to verify lock ownership
  acquired_at TIMESTAMP DEFAULT NOW()
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
CREATE INDEX idx_bots_strategy_key ON bots(strategy_key);
CREATE INDEX idx_bots_elo_rating ON bots(elo_rating);
CREATE INDEX idx_bot_hands_game_id ON bot_hands(game_id);
CREATE INDEX idx_bot_hands_bot_id ON bot_hands(bot_id);
CREATE INDEX idx_bot_locks_game_id ON bot_locks(game_id);
CREATE INDEX idx_bot_locks_acquired_at ON bot_locks(acquired_at);

-- =============================================================================
-- ROW LEVEL SECURITY: Enable RLS on all tables
-- =============================================================================

ALTER TABLE games ENABLE ROW LEVEL SECURITY;
ALTER TABLE game_decks ENABLE ROW LEVEL SECURITY;
ALTER TABLE player_hands ENABLE ROW LEVEL SECURITY;
ALTER TABLE chat_messages ENABLE ROW LEVEL SECURITY;
ALTER TABLE user_elo_ratings ENABLE ROW LEVEL SECURITY;
ALTER TABLE bots ENABLE ROW LEVEL SECURITY;
ALTER TABLE bot_hands ENABLE ROW LEVEL SECURITY;
ALTER TABLE bot_locks ENABLE ROW LEVEL SECURITY;

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

-- Bots: Read-only for authenticated users (for lobby bot selection)
CREATE POLICY "Anyone can view bots" ON bots
  FOR SELECT USING (true);

CREATE POLICY "Only service role can manage bots" ON bots
  FOR ALL USING (auth.role() = 'service_role');

-- Bot hands: ONLY service role can access (edge functions only)
CREATE POLICY "Only service role can access bot hands" ON bot_hands
  FOR ALL USING (auth.role() = 'service_role');

-- Bot locks: ONLY service role can access (edge functions only)
CREATE POLICY "Only service role can access bot locks" ON bot_locks
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
  VALUES (NEW.id, 1000, 0)
  ON CONFLICT (user_id) DO NOTHING;
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

CREATE TRIGGER update_bots_updated_at 
  BEFORE UPDATE ON bots
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_bot_hands_updated_at 
  BEFORE UPDATE ON bot_hands
  FOR EACH ROW 
  EXECUTE FUNCTION update_updated_at_column();

-- Trigger for chat message changes
CREATE TRIGGER handle_chat_messages_changes
  AFTER INSERT OR UPDATE OR DELETE
  ON public.chat_messages
  FOR EACH ROW
  EXECUTE FUNCTION chat_messages_changes();

-- Trigger to create default ELO rating for new users
DROP TRIGGER IF EXISTS handle_new_user_elo_rating ON auth.users;
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
-- SEED DATA: Initial bots with different strategies
-- =============================================================================

INSERT INTO bots (nickname, strategy_key) VALUES
-- Handwritten strategy bots (rule-based)
('0xDEADBEEF', 'handwritten'),
('0x00C0FFEE', 'handwritten'),
('0x00000001', 'handwritten'),
('0x00BEADED', 'handwritten'),

-- Random strategy bots (chaotic)
('0xBABEFACE', 'random'),
('0x000FADED', 'random'),
('0xCAFEBABE', 'random'),
('0xFEEDBEEF', 'random'),
('0x0BADC0DE', 'random'),
('0xFACEFEED', 'random'),
('0x0C4EA7E9', 'random'),

-- One card strategy bots (minimalist efficiency)
('0x0EE0CA5D', 'one_card'),
('0x50105010', 'one_card'),
('0x1A57C45D', 'one_card'),

-- Simple heuristic strategy bots (logical rule-based)
('0x10C1CA11', 'simple_heuristic'),
('0x51A3E357', 'simple_heuristic'),
('0xBA5ED0A5', 'simple_heuristic'),

-- Ultimate champion strategy bots (advanced AI)
('0xCEA4E10E', 'ultimate_champion'),
('0xE1179A7E', 'ultimate_champion'),
('0x0EE51055', 'ultimate_champion'),

-- Champion strategy bots (tournament winners)
('0x11C705E1', 'champion'),
('0x751243A1', 'champion'),
('0xE1179B07', 'champion'),
('0xC0E4EE50', 'champion'),

-- Hacker strategy bots (perfect information - UNFAIR ADVANTAGE)
('0x00000000', 'hacker'),
('0xFFFFFFFF', 'hacker');

-- =============================================================================
-- SETUP COMPLETE!
-- Your database schema is now secure and ready for the game application.
-- Advisory locks are configured for game operation synchronization.
-- Bot system is initialized with sample strategies.
-- =============================================================================
