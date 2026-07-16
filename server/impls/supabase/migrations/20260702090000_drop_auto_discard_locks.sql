-- auto_discard_locks backed the 60-second all-good auto-discard monitor. That
-- timeout is deliberately disabled (actions/good.ts: long-game sessions no
-- longer auto-discard out from under absent attackers), and no TypeScript
-- references the table — it has been pure dead schema (plus an index and RLS
-- policy) since. Remove it; if the timeout ever returns it should ride the
-- games.version CAS like every other transition, not a bespoke lock table.
DROP TABLE IF EXISTS auto_discard_locks CASCADE;
