-- STEP 2 — run ONLY AFTER the new edge functions (CAS + lease) are deployed.
-- Until the new functions are live, the OLD functions still INSERT into these
-- tables, so dropping them earlier would break in-flight games. They are unused
-- and empty once the new code is deployed.
DROP TABLE IF EXISTS game_locks CASCADE;
DROP TABLE IF EXISTS bot_locks  CASCADE;
