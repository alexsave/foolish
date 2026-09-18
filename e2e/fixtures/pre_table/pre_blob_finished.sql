-- The hosted rows that block the contract migration: finished games from before
-- games.state existed.
--
-- HAND WRITTEN (rows.sql beside this is generated and must not be edited). Load
-- after rows.sql. games.state arrived in 20260707120000; a game that reached
-- game_over before it has a JSONB record of itself and no blob, and there has
-- never been one to carry. The hosted database has 34 such rows out of 90, all
-- game_over, created between 2025-07-21 and 2026-07-01. 4a leaves them alone
-- (it synthesises a lobby blob for WAITING rows only), so they arrive at 4c
-- with a roster and a NULL state, which is what the contract migration's
-- precondition refuses.
--
-- The row below is a copy of the captured `finished` game (85133f) with the
-- three columns that arrived with the blob left NULL, exactly as one of those
-- rows looks: state, game_seed and logs_packed. Every dependent row a live game
-- of that era wrote is here too, one per foreign key that points at games, so a
-- test can see which of them the delete takes and which it leaves:
--
--   player_hands, bot_hands, chat_messages, player_views, spectator_views
--                                              ON DELETE CASCADE - they go
--   game_snapshots                             ON DELETE SET NULL - it stays
--
-- The second snapshot below belongs to 85133f, the finished game that DOES have
-- a blob: it is the control that must come through the migration untouched,
-- game_id and all.

INSERT INTO public.games (id, name, deck_length, discard_pile_length, flipped, players, status, power_suit,
                          first_attacker, defender, table_battles, elimination_order, good_timestamp, good_players,
                          state, game_seed, logs_packed, version, round_epoch, created_at, updated_at)
SELECT 'prb001', name, deck_length, discard_pile_length, flipped, players,
       status, power_suit, first_attacker, defender, table_battles, elimination_order, good_timestamp, good_players,
       NULL, NULL, NULL, version, round_epoch, '2025-07-21 00:00:00', '2026-07-01 00:00:00'
FROM public.games WHERE id = '85133f';

-- Membership and the caches the pre-blob server wrote. The view blobs are the
-- ones the captured row carries: this fixture is about which rows survive the
-- delete, not about what a pre-blob envelope looked like.
INSERT INTO public.player_hands (game_id, player_id, joined_at, created_at, updated_at)
SELECT 'prb001', player_id, joined_at, created_at, updated_at FROM public.player_hands WHERE game_id = '85133f';

INSERT INTO public.bot_hands (game_id, bot_id, joined_at, created_at, updated_at)
SELECT 'prb001', bot_id, joined_at, created_at, updated_at FROM public.bot_hands WHERE game_id = '85133f';

INSERT INTO public.player_views (game_id, player_id, view, version, status, updated_at)
SELECT 'prb001', player_id, view, version, status, updated_at FROM public.player_views WHERE game_id = '85133f';

INSERT INTO public.spectator_views (game_id, view, version, status, updated_at)
SELECT 'prb001', view, version, status, updated_at FROM public.spectator_views WHERE game_id = '85133f';

INSERT INTO public.chat_messages (id, game_id, user_id, message, is_system, created_at) VALUES
  ('00000000-0000-4000-8000-0000000000f1', 'prb001', 'dc3c32b8-9486-443b-943c-000000000000', 'gg', false, '2026-07-01 00:00:00');

-- The replay of the deleted game, and the replay of the control.
INSERT INTO public.game_snapshots (id, game_id, player_ids, moves, extras, created_at) VALUES
  ('00000000-0000-4000-8000-0000000000f2', 'prb001',
   '["dc3c32b8-9486-443b-943c-000000000000", "bf8245d5-0270-4aca-8282-000000000001"]'::jsonb,
   '\x0102030405'::bytea, '\x0607'::bytea, '2026-07-01 00:00:00'),
  ('00000000-0000-4000-8000-0000000000f3', '85133f',
   '["dc3c32b8-9486-443b-943c-000000000000", "bf8245d5-0270-4aca-8282-000000000001"]'::jsonb,
   '\x0a0b0c0d0e'::bytea, NULL, '2026-09-17 00:00:00');
