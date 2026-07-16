-- Drop bots whose strategy_key is NOT dispatched by the production bots.wasm.
--
-- champion, ultimate_champion, hacker, espresso, semtex and semtex_max left the
-- shipped bot module (see wasm_choose_move in sdk/c/wasm/wasm_bots_api.c — any
-- unported strat id falls back to random). A seeded bot carrying one of those
-- keys therefore plays as `random`, nothing like its name, and shows up on the
-- Elo leaderboard (/leaderboard) as a misleading "Semtex"/"Champion"/etc. entry.
--
-- seed.sql no longer seeds these (fresh DBs never get them); this removes them
-- from ALREADY-SEEDED live DBs. Their bot_hands rows cascade-delete via the FK,
-- and their leaderboard presence (elo/games accrued while playing as random)
-- goes with them. Guarded for the fresh-reset-before-seed ordering and
-- idempotent (a second run deletes nothing).
DO $$
BEGIN
  IF to_regclass('public.bots') IS NOT NULL THEN
    DELETE FROM bots
    WHERE strategy_key IN (
      'champion', 'ultimate_champion', 'hacker', 'espresso', 'semtex', 'semtex_max'
    );
  END IF;
END;
$$;
