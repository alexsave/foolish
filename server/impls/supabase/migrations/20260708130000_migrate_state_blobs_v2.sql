-- Backfill: rewrite every stored kernel state blob from v1 to v2.
--
-- The deal feature added a deterministic_deck flag byte to the durable state
-- blob (cnitro wasm_state_serialize): v1 was [version=01][put_state...], v2 is
-- [version=02][flag=00/01][put_state...]. The kernel now reads ONLY v2 (no v1
-- read path — that keeps the hot deserialize branch-free), so any game in
-- flight must be converted here rather than tolerated in code.
--
-- games.state is stored as a Postgres bytea-hex TEXT: a literal "\x" prefix then
-- one hex pair per byte (engine.ts bytesToHex). So the version byte is hex chars
-- 3-4, and the payload starts at char 5. Every existing game was dealt on the
-- legacy random-draw path, so the new flag is 0 — they keep drawing exactly as
-- before. Position-based rewrite (copy the "\x" prefix from the row, insert
-- 02 00, keep the payload) so it needs no backslash literal and is idempotent:
-- once a blob is v2 (chars 3-4 = '02') it no longer matches.
--
-- DEPLOY ORDER: run this together with the kernel that writes/reads v2. The old
-- kernel cannot read v2 and the new kernel cannot read v1, so the window between
-- the two is the usual migrate-with-deploy window (a game loaded mid-swap may
-- error and retry); acceptable for this one-time cutover.
UPDATE games
SET state = substr(state, 1, 2) || '0200' || substr(state, 5)
WHERE state IS NOT NULL
  AND substr(state, 3, 2) = '01';
