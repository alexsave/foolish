-- game_snapshots existed only in seed.sql (fresh DBs) — there was never a
-- migration adding it to a live database. finalizeEndedGame() writes here at
-- game end, and the match-history screen reads it, so bring live DBs up to
-- the seed schema. Idempotent: safe on a DB where seed.sql already created it.

CREATE TABLE IF NOT EXISTS game_snapshots (
  id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
  game_id TEXT REFERENCES games(id) ON DELETE SET NULL,
  player_ids JSONB NOT NULL DEFAULT '[]'::jsonb, -- player ids in seat order
  moves BYTEA NOT NULL,
  extras BYTEA,
  created_at TIMESTAMP DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_game_snapshots_game_id ON game_snapshots(game_id);
CREATE INDEX IF NOT EXISTS idx_game_snapshots_created_at ON game_snapshots(created_at);
CREATE INDEX IF NOT EXISTS idx_game_snapshots_player_ids ON game_snapshots USING GIN (player_ids);

ALTER TABLE game_snapshots ENABLE ROW LEVEL SECURITY;

DROP POLICY IF EXISTS "Service role can insert snapshots" ON game_snapshots;
CREATE POLICY "Service role can insert snapshots" ON game_snapshots
  FOR INSERT WITH CHECK ((select auth.role()) = 'service_role');

DROP POLICY IF EXISTS "Participants can read snapshots" ON game_snapshots;
CREATE POLICY "Participants can read snapshots" ON game_snapshots
  FOR SELECT USING (
    (select auth.role()) = 'service_role'
    OR player_ids ? (select auth.uid())::text
  );
