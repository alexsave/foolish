-- create_game: fold the three sequential inserts the `create` edge function did
-- (games → game_decks → player_hands) into ONE transaction / one round-trip.
-- Before this, "create game" paid 3 (originally) serial PostgREST round-trips on
-- top of a cold-start auth — a chunk of the multi-second create latency (#6). The
-- RPC is deliberately dumb: no game logic, just the three inserts the function
-- already did, with column defaults filling in the rest of the games row.
--
-- Idempotent; safe to re-run.
CREATE OR REPLACE FUNCTION create_game(
  p_game_id   TEXT,
  p_name      TEXT,
  p_player_id UUID,
  p_players   JSONB
) RETURNS VOID
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  INSERT INTO games (id, name, players, status)
    VALUES (p_game_id, p_name, p_players, 'waiting');

  INSERT INTO game_decks (game_id, deck)
    VALUES (p_game_id, '[]'::jsonb);

  -- player_hands doubles as the player↔game relationship row.
  INSERT INTO player_hands (game_id, player_id, hand, awaiting_attack)
    VALUES (p_game_id, p_player_id, '[]'::jsonb, false);
END;
$$;
