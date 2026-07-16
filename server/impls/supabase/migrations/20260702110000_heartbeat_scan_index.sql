-- The bot-heartbeat SCAN runs every 10 seconds:
--   SELECT ... FROM games WHERE status = 'playing' AND updated_at < t1 AND updated_at > t2
-- A narrow partial index serves it with one range scan over only the live
-- games instead of combining the separate status and updated_at indexes.
CREATE INDEX IF NOT EXISTS idx_games_playing_updated_at
  ON games(updated_at) WHERE status = 'playing';
