-- Realtime: retire the user-{email local part} private channel policies.
--
-- Nothing joins or broadcasts a user- topic. The web's last `user-${username}`
-- subscription was removed in 2025-07 (2b3fc9c1), and no server, iOS or kernel
-- code names the topic. The three policies stayed behind, and two of them were
-- holes:
--
--   "authenticated can receive private messages" matched the topic against the
--   local part of the caller's email, so alice@a.com and alice@b.com were each
--   authorized for the other's user-alice.
--
--   "authenticated can send private messages" let any signed-in user broadcast
--   to any user- topic.
--
-- The service-role INSERT policy goes with them: service_role bypasses RLS, and
-- there is no user- sender to serve.
--
-- Like the gu-/chat: policies (20260917120000), these were never created by a
-- migration, only by seed.sql and a hosted copy, hence DROP IF EXISTS. seed.sql
-- no longer creates them; e2e/realtime_channel_auth.test.ts asserts the two
-- paths end with the same policies.

DROP POLICY IF EXISTS "authenticated can receive private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "authenticated can send private messages" ON "realtime"."messages";
DROP POLICY IF EXISTS "service role can send private messages" ON "realtime"."messages";
