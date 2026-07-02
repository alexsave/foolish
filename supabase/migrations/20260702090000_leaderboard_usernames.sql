-- Leaderboard support: put the display name on user_elo_ratings.
--
-- user_elo_ratings has always been publicly readable (its SELECT policy is
-- USING (true)) and indexed on elo_rating, but a client could never render a
-- standings table from it: the only identity on the row is user_id, and
-- auth.users (where the username lives, in raw_user_meta_data) is not client
-- readable. Denormalizing the username onto the rating row closes that gap;
-- the trigger below fires on metadata updates as well, so the copy follows any
-- rename made outside the app (GoTrue updateUser, admin dashboard).

ALTER TABLE user_elo_ratings ADD COLUMN IF NOT EXISTS username TEXT;

-- New signups: stamp the username onto the auto-created rating row. Same
-- function seed.sql installs; CREATE OR REPLACE keeps live DBs identical.
CREATE OR REPLACE FUNCTION public.create_default_elo_rating()
RETURNS TRIGGER
SECURITY DEFINER
SET search_path = ''
LANGUAGE plpgsql
AS $$
BEGIN
  INSERT INTO public.user_elo_ratings (user_id, elo_rating, games_played, username)
  VALUES (NEW.id, 1000, 0, NEW.raw_user_meta_data->>'username')
  ON CONFLICT (user_id) DO UPDATE SET username = EXCLUDED.username;
  RETURN NEW;
END;
$$;

-- Fire on metadata UPDATE too: the app has no rename flow, but GoTrue's
-- updateUser (and the admin dashboard) can change raw_user_meta_data, and the
-- denormalized copy must follow.
DROP TRIGGER IF EXISTS handle_new_user_elo_rating ON auth.users;
CREATE TRIGGER handle_new_user_elo_rating
  AFTER INSERT OR UPDATE OF raw_user_meta_data
  ON auth.users
  FOR EACH ROW
  EXECUTE FUNCTION create_default_elo_rating();

-- Existing users: backfill from auth metadata.
UPDATE user_elo_ratings r
SET username = u.raw_user_meta_data->>'username'
FROM auth.users u
WHERE u.id = r.user_id AND r.username IS NULL;

-- The leaderboard query is ORDER BY elo_rating DESC filtered to players with
-- at least one game; idx_user_elo_ratings_elo_rating already covers the sort.
