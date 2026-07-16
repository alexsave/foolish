-- The packed kernel blob in games.state is the UNMASKED volatile state —
-- every player's hand and the exact deck order. Clients get their view
-- through the get_game edge function (service role), which serializes a
-- per-viewer MASKED blob inside the C kernel (sdk/c/src/view.c,
-- docs/PACKED_WIRE_CUTOVER.md). But the "Anyone can view games" RLS policy
-- exposed the whole row — including state — through PostgREST, so any
-- authenticated client could fetch and decode every hand. Row-level
-- security cannot hide a column; switch games to column-level SELECT
-- grants: everything except the blob and the bot-lease bookkeeping (which
-- no client reads).
--
-- The web client stopped reading games directly at the state-blob cutover
-- (ServerContext goes through get_game / get_my_games), so nothing
-- client-side selects these columns anymore; edge functions use the
-- service role and are unaffected.
REVOKE SELECT ON public.games FROM anon, authenticated;
GRANT SELECT (
  id, name, deck_length, discard_pile_length, flipped, players, status,
  power_suit, first_attacker, defender, table_battles, elimination_order,
  good_timestamp, good_players, version, created_at, updated_at
) ON public.games TO anon, authenticated;
