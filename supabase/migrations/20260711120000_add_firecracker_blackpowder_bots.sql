-- Add the two missing SHIPPED difficulty-ladder rungs — firecracker (Medium)
-- and blackpowder (Hard), Durak Bot Ordnance Chart — as playable bots on an
-- ALREADY-SEEDED (live) database.
--
-- The full schema/seed lives in supabase/seed.sql, which only stands a fresh DB
-- up from scratch; this migration applies the same incremental change to a DB
-- that already has the schema + data (we do NOT re-run seed.sql). Both C
-- strategies (cnitro/src/firecracker_strategy.c, blackpowder_strategy.c) and the
-- bots.wasm dispatch (wasm_choose_move) already ship these; this row insert is
-- what makes them selectable in the lobby / dealt into games.
--
-- Guarded + idempotent:
--   * to_regclass guard: on a fresh `db reset` migrations run BEFORE seed.sql
--     (bots table doesn't exist yet), so this is a clean no-op there — fresh-DB
--     bots come from seed.sql, which also applies the '%' prefix.
--   * NOT EXISTS on nickname: re-running never double-inserts.
--
-- Bots carry the reserved '%' prefix (see 20260615120000) so bot-vs-human stays
-- recoverable from the name-only replay codec.
DO $$
BEGIN
  IF to_regclass('public.bots') IS NOT NULL THEN
    INSERT INTO bots (nickname, strategy_key)
    SELECT v.nickname, v.strategy_key
    FROM (VALUES
      ('%Firecracker 1', 'firecracker'),
      ('%Firecracker 2', 'firecracker'),
      ('%Firecracker 3', 'firecracker'),
      ('%Blackpowder 1', 'blackpowder'),
      ('%Blackpowder 2', 'blackpowder'),
      ('%Blackpowder 3', 'blackpowder')
    ) AS v(nickname, strategy_key)
    WHERE NOT EXISTS (
      SELECT 1 FROM bots b WHERE b.nickname = v.nickname
    );
  END IF;
END;
$$;
