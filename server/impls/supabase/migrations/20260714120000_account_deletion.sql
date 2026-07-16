-- Account deletion support (docs/ORACLE_MONETIZATION_ENGINEERING.md §4;
-- App Store Guideline 5.1.1(v); Google Play data-deletion policy).
--
-- The `delete-account` edge function calls this to scrub the caller's personal
-- data from SHARED game history, then deletes the auth.users row via the GoTrue
-- admin API. Rows the user solely owns (player_hands, player_views,
-- user_elo_ratings, …) already carry ON DELETE CASCADE from auth.users, so the
-- admin delete removes them; this function only handles PII that lives in rows
-- OTHER players also reference — the player's display name embedded in
-- games.players — plus a belt-and-suspenders scrub of the denormalized username
-- copy on user_elo_ratings (in case that FK is ever SET NULL rather than CASCADE).
--
-- Not scrubbed here: the player-name blob inside game_snapshots.extras (the
-- replay codec). Anonymizing it requires re-encoding the replay; the design doc
-- treats that as a follow-up ("replays keep the codec's player-name blob only if
-- anonymized"). This function does NOT delete shared game rows — only redacts the
-- leaving player's identity from them.

CREATE OR REPLACE FUNCTION public.delete_account(p_user_id UUID)
RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  -- Redact the player's display name in every game they appear in. players is a
  -- JSONB array of {player_id, name, …}; rebuild it, anonymizing only the
  -- leaving player's element. The @> filter limits the rewrite to affected rows.
  UPDATE games g
  SET players = (
    SELECT jsonb_agg(
      CASE
        WHEN elem->>'player_id' = p_user_id::text
          THEN jsonb_set(elem, '{name}', '"Deleted player"'::jsonb)
        ELSE elem
      END
    )
    FROM jsonb_array_elements(g.players) AS elem
  )
  WHERE g.players @> jsonb_build_array(jsonb_build_object('player_id', p_user_id::text));

  -- Belt-and-suspenders: clear the denormalized username copy. If the FK is
  -- ON DELETE CASCADE the row vanishes with the auth user anyway; if it is ever
  -- SET NULL, this ensures no username lingers.
  UPDATE public.user_elo_ratings
  SET username = NULL
  WHERE user_id = p_user_id;
END;
$$;

-- Only the service role (the edge function) may run this — never a client.
REVOKE ALL ON FUNCTION public.delete_account(UUID) FROM PUBLIC;
REVOKE ALL ON FUNCTION public.delete_account(UUID) FROM anon;
REVOKE ALL ON FUNCTION public.delete_account(UUID) FROM authenticated;
GRANT EXECUTE ON FUNCTION public.delete_account(UUID) TO service_role;
