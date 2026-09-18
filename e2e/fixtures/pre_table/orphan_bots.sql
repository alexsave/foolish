-- The hosted rows that failed the expand migration: bot seats whose bots row is gone.
--
-- HAND WRITTEN (rows.sql beside this is generated and must not be edited). Load
-- after rows.sql. It reproduces, on top of the captured games, what
-- 20260711130000_drop_non_wasm_bots.sql left on hosted: seats in games.players
-- that name a retired strategy's bot, whose `bots` row was deleted six months
-- ago. The hosted database has 29 such seats across 21 games (20 waiting, 9
-- game_over, none playing), with names like '%One Card 2', '%Champion 1',
-- '%Champion 4', '%Espresso 3' and '%Cordite Max 2'.
--
-- The three games below are copies of captured ones with one bot seat repointed
-- at a retired bot, one per status the pre-pass has a branch for. The retired
-- bots are seated first and then deleted exactly as that migration deleted
-- them, so the bot_hands rows go the way they went on hosted: by the
-- bot_hands -> bots cascade, not by hand.

INSERT INTO public.bots (id, nickname, strategy_key, created_at, updated_at) VALUES
  ('00000000-0000-4000-8000-0000000000c1', '%Champion 1',    'champion',    '2026-09-17 00:00:00', '2026-09-17 00:00:00'),
  ('00000000-0000-4000-8000-0000000000e3', '%Espresso 3',    'espresso',    '2026-09-17 00:00:00', '2026-09-17 00:00:00'),
  ('00000000-0000-4000-8000-0000000000c2', '%Cordite Max 2', 'semtex_max',  '2026-09-17 00:00:00', '2026-09-17 00:00:00'),
  ('00000000-0000-4000-8000-0000000000d1', '%One Card 2',    'one_card',    '2026-09-17 00:00:00', '2026-09-17 00:00:00');

-- waiting: the 3-seat lobby (a human and two bots), its second bot retired.
INSERT INTO public.games (id, name, deck_length, discard_pile_length, flipped, players, status, power_suit,
                          first_attacker, defender, table_battles, elimination_order, good_timestamp, good_players,
                          state, game_seed, logs_packed, version, round_epoch, created_at, updated_at)
SELECT 'worp01', name, deck_length, discard_pile_length, flipped,
       jsonb_set(jsonb_set(players, '{2,player_id}', '"00000000-0000-4000-8000-0000000000c1"'),
                 '{2,name}', '"%Champion 1"'),
       status, power_suit, first_attacker, defender, table_battles, elimination_order, good_timestamp, good_players,
       state, game_seed, logs_packed, version, round_epoch, created_at, updated_at
FROM public.games WHERE id = '779adf';

-- waiting, BOTH bot seats retired. This is the row that broke the first deploy
-- of this migration on hosted (game 0370d0 seated %One Card 2 and %Champion 1):
-- resolving one seat rewrites games.players, which is what the bridge trigger
-- fires on, and the re-derivation then met the seat still waiting its turn.
INSERT INTO public.games (id, name, deck_length, discard_pile_length, flipped, players, status, power_suit,
                          first_attacker, defender, table_battles, elimination_order, good_timestamp, good_players,
                          state, game_seed, logs_packed, version, round_epoch, created_at, updated_at)
SELECT 'worp04', name, deck_length, discard_pile_length, flipped,
       jsonb_set(jsonb_set(
         jsonb_set(jsonb_set(players, '{1,player_id}', '"00000000-0000-4000-8000-0000000000d1"'),
                   '{1,name}', '"%One Card 2"'),
         '{2,player_id}', '"00000000-0000-4000-8000-0000000000c1"'),
         '{2,name}', '"%Champion 1"'),
       status, power_suit, first_attacker, defender, table_battles, elimination_order, good_timestamp, good_players,
       state, game_seed, logs_packed, version, round_epoch, created_at, updated_at
FROM public.games WHERE id = '779adf';

-- game_over: the finished 3-seat game, its bot seat retired. The state blob is
-- the record of it and seats three, so the seat cannot go.
INSERT INTO public.games (id, name, deck_length, discard_pile_length, flipped, players, status, power_suit,
                          first_attacker, defender, table_battles, elimination_order, good_timestamp, good_players,
                          state, game_seed, logs_packed, version, round_epoch, created_at, updated_at)
SELECT 'gorp01', name, deck_length, discard_pile_length, flipped,
       jsonb_set(jsonb_set(players, '{2,player_id}', '"00000000-0000-4000-8000-0000000000e3"'),
                 '{2,name}', '"%Espresso 3"'),
       status, power_suit, first_attacker, defender, table_battles, elimination_order, good_timestamp, good_players,
       state, game_seed, logs_packed, version, round_epoch, created_at, updated_at
FROM public.games WHERE id = '85133f';

-- playing: the 4-seat mid-game (two humans, two bots), its first bot retired.
-- Hosted has none of these today; this is the row the pre-pass's playing branch
-- exists for.
INSERT INTO public.games (id, name, deck_length, discard_pile_length, flipped, players, status, power_suit,
                          first_attacker, defender, table_battles, elimination_order, good_timestamp, good_players,
                          state, game_seed, logs_packed, version, round_epoch, created_at, updated_at)
SELECT 'plorp1', name, deck_length, discard_pile_length, flipped,
       jsonb_set(jsonb_set(players, '{2,player_id}', '"00000000-0000-4000-8000-0000000000c2"'),
                 '{2,name}', '"%Cordite Max 2"'),
       status, power_suit, first_attacker, defender, table_battles, elimination_order, good_timestamp, good_players,
       state, game_seed, logs_packed, version, round_epoch, created_at, updated_at
FROM public.games WHERE id = 'a84886';

-- The membership rows the live server wrote, the retired bots' among them.
INSERT INTO public.bot_hands (game_id, bot_id, joined_at, created_at, updated_at) VALUES
  ('worp01', 'dfdca6c5-d4e9-4a5a-94dc-000000000005', '2026-09-17 00:00:00', '2026-09-17 00:00:00', '2026-09-17 00:00:00'),
  ('worp01', '00000000-0000-4000-8000-0000000000c1', '2026-09-17 00:00:00', '2026-09-17 00:00:00', '2026-09-17 00:00:00'),
  ('gorp01', '00000000-0000-4000-8000-0000000000e3', '2026-09-17 00:00:00', '2026-09-17 00:00:00', '2026-09-17 00:00:00'),
  ('plorp1', 'a9e7a7b0-8c30-4808-8ce7-000000000007', '2026-09-17 00:00:00', '2026-09-17 00:00:00', '2026-09-17 00:00:00'),
  ('plorp1', '00000000-0000-4000-8000-0000000000c2', '2026-09-17 00:00:00', '2026-09-17 00:00:00', '2026-09-17 00:00:00');

-- And the retirement itself: the bots go, their bot_hands rows cascade with
-- them, and the seats in games.players are left naming nothing.
DELETE FROM public.bots WHERE strategy_key IN ('champion', 'espresso', 'semtex_max', 'one_card');
