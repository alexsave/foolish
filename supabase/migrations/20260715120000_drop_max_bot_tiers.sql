-- Retire the `_max` bot tiers: cordite_max, octogen_max (and semtex_max, if an
-- old DB still carries it — 20260711130000_drop_non_wasm_bots removed those).
--
-- Neither tier was a distinct bot:
--
--   * octogen_max was a straight ALIAS of octogen. Both registered STRAT.octogen
--     with the identical OG_TRUMP_KEEP env and no budget knob — bot_strategy.ts
--     even said so ("the _max keys alias the base strategy until a kernel-side
--     max-budget knob exists"). Two names, one brain, one leaderboard row each.
--
--   * cordite_max was cordite with CD_BUDGET=max. That mode is a FLAT world
--     budget (W1=120/W2=240/W3=168, cordite_strategy.c cd_worlds) while the
--     `prod` schedule cordite uses is player-count-aware (240/480/336 at 6
--     players). So "Max" only out-sampled plain Cordite at 2-4 players and ran
--     at roughly HALF its budget at 6-8 — i.e. the bot advertised as the
--     stronger tier was the weaker one in the bigger games. The roster keeps the
--     prod budget under the single `cordite` key.
--
-- The canonical roster is now the C table (sdk/c/src/bot_roster.c); these keys
-- are absent from it, so a bot still carrying one would fall through to `random`
-- and play nothing like its name — the same failure mode 20260711130000 fixed.
--
-- Deleting the rows (rather than remapping them onto cordite/octogen) follows
-- that precedent: their bot_hands rows cascade-delete via the FK, and the Elo
-- they accrued goes with them. Remapping would instead leave live bots named
-- "%Cordite Max 1" playing plain cordite, which is the naming lie we are
-- removing. Historical replay blobs are unaffected — they embed names at encode
-- time, and the iOS nickname parser keeps a "Max" suffix entry for exactly that
-- reason (docs/IOS_BOT_NAMING.md §2).
--
-- Guarded for the fresh-reset-before-seed ordering and idempotent (a second run
-- deletes nothing).
DO $$
BEGIN
  IF to_regclass('public.bots') IS NOT NULL THEN
    DELETE FROM bots
    WHERE strategy_key IN ('cordite_max', 'octogen_max', 'semtex_max');
  END IF;
END;
$$;
