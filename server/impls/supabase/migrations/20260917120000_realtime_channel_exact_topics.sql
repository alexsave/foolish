-- Realtime: authorize the gu- and chat: private channels by exact topic.
--
-- The web joins `gu-{game_id}-{user_id}` (its per-seat animation stream) and
-- `chat:{game_id}` as private channels, and Realtime admits a private join only
-- if a SELECT policy on realtime.messages passes for that topic.
--
-- The gu- policy took the topic apart with split_part(topic, '-', n). A user id
-- is a hyphenated UUID, so split_part(topic, '-', 3) is only its first 8 hex
-- digits and never equals auth.uid()::text. Every gu- join was refused, the
-- owner's included: the server's per-seat animation broadcasts reached nobody,
-- and a seated web player's board did not update from other players' or bots'
-- moves. It failed closed, so nothing leaked.
--
-- The chat: policy had the looser form of the same shape: split_part(topic,
-- ':', 2) authorized any `chat:{game_id}:<anything>` for a member of the game.
--
-- Both now rebuild the exact topic from the caller's own membership row, so a
-- topic is authorized only if it is character-for-character the one the server
-- broadcasts to for a game the caller is in. That holds for any game id,
-- hyphenated or not: the game id is read from player_hands, never parsed out of
-- the topic.
--
-- These policies were never created by a migration: seed.sql defines them and
-- the hosted project carries a copy. DROP IF EXISTS + CREATE lands the same end
-- state on both. seed.sql carries the identical definitions, and
-- e2e/realtime_channel_auth.test.ts asserts the two agree and evaluates the
-- policies the way Realtime does.

DROP POLICY IF EXISTS "authenticated can receive game-user messages" ON "realtime"."messages";
CREATE POLICY "authenticated can receive game-user messages"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'gu-%' AND
  EXISTS (
    SELECT 1
    FROM public.player_hands ph
    WHERE ph.player_id = (SELECT auth.uid())
      AND (SELECT realtime.topic()) = 'gu-' || ph.game_id || '-' || (SELECT auth.uid())::text
  ) AND
  realtime.messages.extension IN ('broadcast')
);

DROP POLICY IF EXISTS "authenticated can receive chat broadcasts" ON "realtime"."messages";
CREATE POLICY "authenticated can receive chat broadcasts"
ON "realtime"."messages"
FOR SELECT
TO authenticated
USING (
  (SELECT realtime.topic()) LIKE 'chat:%' AND
  EXISTS (
    SELECT 1
    FROM public.player_hands ph
    WHERE ph.player_id = (SELECT auth.uid())
      AND (SELECT realtime.topic()) = 'chat:' || ph.game_id
  ) AND
  realtime.messages.extension IN ('broadcast')
);
