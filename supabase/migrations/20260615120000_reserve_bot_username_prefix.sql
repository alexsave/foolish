-- Reserve the bot-name prefix ('%') on an ALREADY-SEEDED (live) database.
--
-- The full schema/seed lives in supabase/seed.sql, which is only used to stand a
-- fresh DB up from scratch. This migration applies the same change incrementally
-- to a database that already has the schema + data, so we do NOT re-run seed.sql.
-- Two parts, both idempotent and safe to re-run:
--   1. the auth.users guard trigger (humans may not use '%' in a username)
--   2. rename existing bots to carry the '%' prefix
--
-- ⚠️ Before applying, confirm no human has already taken a '%' username:
--   SELECT id, raw_user_meta_data->>'username' AS username
--   FROM auth.users
--   WHERE position('%' in coalesce(raw_user_meta_data->>'username','')) > 0;
-- Any rows there would be blocked on their next update once the trigger is live.

-- 1. Human usernames may not contain the reserved bot prefix. This trigger is the
--    AUTHORITATIVE guard (the client-side check in AuthContext is only for fast
--    UX and is bypassable). position() is a literal substring search, so '%' is
--    the character here, not a LIKE wildcard.
CREATE OR REPLACE FUNCTION public.enforce_username_not_bot()
RETURNS TRIGGER
SECURITY DEFINER
SET search_path = ''
LANGUAGE plpgsql
AS $$
BEGIN
  IF position('%' in coalesce(NEW.raw_user_meta_data->>'username', '')) > 0 THEN
    RAISE EXCEPTION 'username may not contain the reserved bot prefix (%%)'
      USING ERRCODE = 'check_violation';
  END IF;
  RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS enforce_username_not_bot ON auth.users;
CREATE TRIGGER enforce_username_not_bot
  BEFORE INSERT OR UPDATE ON auth.users
  FOR EACH ROW
  EXECUTE FUNCTION public.enforce_username_not_bot();

-- 2. Prefix existing bot nicknames so bot-vs-human is recoverable from the
--    name-only replay codec. Guarded so a fresh `db reset` (migrations run
--    BEFORE seed.sql, when the bots table doesn't exist yet) is a clean no-op —
--    fresh-DB bots get the prefix from seed.sql instead. The left() check makes
--    it idempotent, so re-running never double-prefixes.
DO $$
BEGIN
  IF to_regclass('public.bots') IS NOT NULL THEN
    UPDATE bots SET nickname = '%' || nickname WHERE left(nickname, 1) <> '%';
  END IF;
END;
$$;
