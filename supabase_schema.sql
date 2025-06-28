-- Supabase Schema for Game Application
-- Run this after initializing your Supabase project

-- Drop everything first (dev environment)
DROP TABLE IF EXISTS chat_messages CASCADE;
DROP TABLE IF EXISTS private_user_channel CASCADE;
DROP TABLE IF EXISTS public_game_channel CASCADE;
DROP TABLE IF EXISTS player_games CASCADE;
DROP TABLE IF EXISTS games CASCADE;
DROP TABLE IF EXISTS users CASCADE;
DROP FUNCTION IF EXISTS public.handle_new_user() CASCADE;
DROP FUNCTION IF EXISTS public.get_user_games(TEXT) CASCADE;
DROP FUNCTION IF EXISTS update_updated_at_column() CASCADE;
DROP TRIGGER IF EXISTS on_auth_user_created ON auth.users;
DROP TRIGGER IF EXISTS update_users_updated_at ON users;
DROP TRIGGER IF EXISTS update_games_updated_at ON games;
DROP TYPE IF EXISTS game_status CASCADE;
DROP TYPE IF EXISTS player_status CASCADE;

-- Drop existing RLS policies
DROP POLICY IF EXISTS "Users can view all users" ON users;
DROP POLICY IF EXISTS "Users can insert themselves" ON users;
DROP POLICY IF EXISTS "Users can update own record" ON users;
DROP POLICY IF EXISTS "Users can view games they're in" ON games;
DROP POLICY IF EXISTS "Anyone can create games" ON games;
DROP POLICY IF EXISTS "Players can update games they're in" ON games;
DROP POLICY IF EXISTS "Users can view their game memberships" ON player_games;
DROP POLICY IF EXISTS "Users can join games" ON player_games;
DROP POLICY IF EXISTS "Users can leave games" ON player_games;
DROP POLICY IF EXISTS "Players can view game messages" ON public_game_channel;
DROP POLICY IF EXISTS "Players can send game messages" ON public_game_channel;
DROP POLICY IF EXISTS "Users can view own private messages" ON private_user_channel;
DROP POLICY IF EXISTS "Users can receive private messages" ON private_user_channel;
DROP POLICY IF EXISTS "Players can view chat messages for their games" ON chat_messages;
DROP POLICY IF EXISTS "Players can send chat messages to their games" ON chat_messages;

-- Enable necessary extensions
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- Create custom types for better type safety
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

-- Users table
CREATE TABLE users (
  id TEXT PRIMARY KEY,
  name TEXT UNIQUE NOT NULL,
  created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
  updated_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
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
  player_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
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
  user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  message JSONB NOT NULL, -- Message object
  created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

-- Chat messages table
CREATE TABLE chat_messages (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  message TEXT NOT NULL,
  is_system BOOLEAN NOT NULL DEFAULT false,
  created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

-- Create indexes for better performance
CREATE INDEX idx_users_name ON users(name);
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
ALTER TABLE users ENABLE ROW LEVEL SECURITY;
ALTER TABLE games ENABLE ROW LEVEL SECURITY;
ALTER TABLE player_games ENABLE ROW LEVEL SECURITY;
ALTER TABLE public_game_channel ENABLE ROW LEVEL SECURITY;
ALTER TABLE private_user_channel ENABLE ROW LEVEL SECURITY;
ALTER TABLE chat_messages ENABLE ROW LEVEL SECURITY;

-- RLS Policies

-- Users: can read all users, can only update own record
CREATE POLICY "Users can view all users" ON users
  FOR SELECT USING (true);

CREATE POLICY "Users can insert themselves" ON users
  FOR INSERT WITH CHECK (true);

CREATE POLICY "Users can update own record" ON users
  FOR UPDATE USING (auth.uid()::text = id);

-- Games: players can view games they're in, anyone can create
CREATE POLICY "Users can view games they're in" ON games
  FOR SELECT USING (
    id IN (
      SELECT game_id FROM player_games 
      WHERE player_id = auth.uid()::text
    )
  );

CREATE POLICY "Anyone can create games" ON games
  FOR INSERT WITH CHECK (true);

CREATE POLICY "Players can update games they're in" ON games
  FOR UPDATE USING (
    id IN (
      SELECT game_id FROM player_games 
      WHERE player_id = auth.uid()::text
    )
  );

-- Player-games: players can view their own relationships
CREATE POLICY "Users can view their game memberships" ON player_games
  FOR SELECT USING (player_id = auth.uid()::text);

CREATE POLICY "Users can join games" ON player_games
  FOR INSERT WITH CHECK (player_id = auth.uid()::text);

CREATE POLICY "Users can leave games" ON player_games
  FOR DELETE USING (player_id = auth.uid()::text);

-- Public game channel: players can view messages for games they're in
CREATE POLICY "Players can view game messages" ON public_game_channel
  FOR SELECT USING (
    game_id IN (
      SELECT game_id FROM player_games 
      WHERE player_id = auth.uid()::text
    )
  );

CREATE POLICY "Players can send game messages" ON public_game_channel
  FOR INSERT WITH CHECK (
    game_id IN (
      SELECT game_id FROM player_games 
      WHERE player_id = auth.uid()::text
    )
  );

-- Private user channel: users can only see their own messages
CREATE POLICY "Users can view own private messages" ON private_user_channel
  FOR SELECT USING (user_id = auth.uid()::text);

CREATE POLICY "Users can receive private messages" ON private_user_channel
  FOR INSERT WITH CHECK (user_id = auth.uid()::text);

-- Chat messages: players can view messages for games they're in
CREATE POLICY "Players can view chat messages for their games" ON chat_messages
  FOR SELECT USING (
    game_id IN (
      SELECT game_id FROM player_games 
      WHERE player_id = auth.uid()::text
    )
  );

CREATE POLICY "Players can send chat messages to their games" ON chat_messages
  FOR INSERT WITH CHECK (
    game_id IN (
      SELECT game_id FROM player_games 
      WHERE player_id = auth.uid()::text
    ) AND user_id = auth.uid()::text
  );

-- Functions for automatic updated_at timestamps
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ language 'plpgsql';

-- Create triggers for updated_at
CREATE TRIGGER update_users_updated_at BEFORE UPDATE ON users
    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_games_updated_at BEFORE UPDATE ON games
    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();

-- Helper function to get user's games
CREATE OR REPLACE FUNCTION get_user_games(user_id_param TEXT)
RETURNS TABLE(game_id TEXT, game_data JSONB) AS $$
BEGIN
  RETURN QUERY
  SELECT g.id as game_id, to_jsonb(g.*) as game_data
  FROM games g
  INNER JOIN player_games pg ON g.id = pg.game_id
  WHERE pg.player_id = user_id_param;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- New user trigger function (ready for future use)
CREATE FUNCTION public.handle_new_user()
RETURNS trigger
LANGUAGE plpgsql
SECURITY DEFINER SET search_path = ''
AS $$
BEGIN
  -- Create a user record in our users table
  INSERT INTO public.users (
    id,
    name
  )
  VALUES (
    new.id,
    COALESCE(new.raw_user_meta_data->>'name', 'Player' || substr(new.id, 1, 8))
  );

  -- Return the newly created user
  RETURN new;
END;
$$;

-- Trigger the function every time a user is created
CREATE TRIGGER on_auth_user_created
  AFTER INSERT ON auth.users
  FOR EACH ROW EXECUTE PROCEDURE public.handle_new_user();
