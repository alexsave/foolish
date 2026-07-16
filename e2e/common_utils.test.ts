// common_utils.ts is imported by BOTH the server and the client (it is
// deliberately kernel-free). The e2e game flow exercises its hot paths, but
// the guard/throw branches, the spectator projection, and the scoring helpers
// (ELO, rankings) went untested. These are all pure functions.
//
// Pure test — needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  cloneGame, get_next_player_index, canCover, cardDisplay, card_comp, getCardValue,
  validate_defender_status, verify_card_array, verify_cards_in_players_hand,
  game_done, verify_player_in_game, other_player, personalize_game,
  calculateEloChange, calculateGameRankings, addLog,
} from '../supabase/functions/_shared/common/common_utils.ts';
import {
  Game, PrivatePlayer, Card, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY, LOG_TYPE,
  PersonalGame,
} from '../supabase/functions/_shared/core/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const C = (suit: number, value: number): Card => ({ suit, value });

const mkPlayer = (i: number, hand: Card[], status = PLAYER_STATUS.IN): PrivatePlayer => ({
  player_id: `p${i}`, name: `P${i}`, status, is_ai: false,
  hand, awaiting_attack: false, hand_length: hand.length,
  strategy_key: STRATEGY_KEY.RANDOM,
});

const mkGame = (players: PrivatePlayer[]): Game => ({
  players, deck: [], logs: [], id: 'cu', name: 'cu', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: C(3, 6), power_suit: 3,
  first_attacker: 0, defender: 1, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
});

test('cloneGame produces an independent deep copy', () => {
  const g = mkGame([mkPlayer(0, [C(0, 5)]), mkPlayer(1, [C(1, 6)])]);
  g.table_battles = [{ attack: C(0, 5), defense: null }];
  g.logs = [{ id: 'l', created_at: 't', game_id: 'cu', log_type: LOG_TYPE.ATTACK, player_id: 'p0', card_pairs: [{ primary: C(0, 5), target: null }], defender_index: null }];
  const c = cloneGame(g);
  c.players[0].hand[0].value = 99;
  c.table_battles[0].attack.value = 88;
  c.logs[0].card_pairs[0].primary.value = 77;
  assert.equal(g.players[0].hand[0].value, 5, 'hand not aliased');
  assert.equal(g.table_battles[0].attack.value, 5, 'battle not aliased');
  assert.equal(g.logs[0].card_pairs[0].primary.value, 5, 'log pair not aliased');
});

test('get_next_player_index skips OUT seats and guards a one-player table', () => {
  const g = mkGame([mkPlayer(0, []), mkPlayer(1, [], PLAYER_STATUS.OUT), mkPlayer(2, [])]);
  assert.equal(get_next_player_index(g, 0), 2, 'skips the OUT seat between');
  // Only one IN player -> returns current (the game-should-have-ended guard).
  const solo = mkGame([mkPlayer(0, []), mkPlayer(1, [], PLAYER_STATUS.OUT)]);
  assert.equal(get_next_player_index(solo, 0), 0, 'returns current when one remains');
});

test('canCover / card_comp / getCardValue / cardDisplay behave', () => {
  assert.ok(canCover(C(0, 7), C(0, 9), 3), 'higher same suit covers');
  assert.ok(!canCover(C(0, 7), C(0, 6), 3), 'lower same suit does not');
  assert.ok(canCover(C(0, 7), C(3, 5), 3), 'trump covers off-suit');
  assert.ok(!canCover(C(3, 7), C(0, 9), 3), 'off-suit cannot cover a trump');
  assert.ok(card_comp(C(1, 5), C(1, 5)) && !card_comp(C(1, 5), C(2, 5)), 'card_comp exact match');
  assert.equal(getCardValue(C(3, 5), 3), 25, 'trump gets the +20 bonus');
  assert.equal(getCardValue(C(0, 5), 3), 5, 'non-trump has no bonus');
  assert.equal(typeof cardDisplay(C(0, 13)), 'string', 'cardDisplay renders a name');
});

test('validate_defender_status accepts and rejects both directions', () => {
  const g = mkGame([mkPlayer(0, []), mkPlayer(1, [])]);  // defender = seat 1 = p1
  assert.doesNotThrow(() => validate_defender_status(g, 'p1', true), 'p1 is the defender');
  assert.doesNotThrow(() => validate_defender_status(g, 'p0', false), 'p0 is not the defender');
  assert.throws(() => validate_defender_status(g, 'p0', true), /is not the defender/, 'p0 required-defender rejected');
  assert.throws(() => validate_defender_status(g, 'p1', false), /is the defender/, 'p1 required-non-defender rejected');
});

test('verify_card_array / verify_cards_in_players_hand reject bad payloads', () => {
  assert.throws(() => verify_card_array('x' as unknown, 'cards'), /must be an array/);
  assert.throws(() => verify_card_array([{ suit: 0 }] as unknown, 'cards'), /invalid card/);
  assert.doesNotThrow(() => verify_card_array([C(0, 5)], 'cards'));

  const p = mkPlayer(0, [C(0, 5), C(1, 6)]);
  assert.doesNotThrow(() => verify_cards_in_players_hand(p, [C(0, 5)]), 'in-hand card ok');
  assert.throws(() => verify_cards_in_players_hand(p, [C(2, 9)]), /is not in player/, 'missing card rejected');
});

test('game_done reports the lone survivor, else null', () => {
  const live = mkGame([mkPlayer(0, []), mkPlayer(1, [])]);
  assert.equal(game_done(live), null, 'two IN -> not done');
  const done = mkGame([mkPlayer(0, [], PLAYER_STATUS.IN), mkPlayer(1, [], PLAYER_STATUS.OUT)]);
  assert.equal(game_done(done), 'p0', 'one IN, rest OUT -> that player');
});

test('verify_player_in_game and other_player', () => {
  const g = mkGame([mkPlayer(0, [C(0, 5)]), mkPlayer(1, [])]);
  assert.doesNotThrow(() => verify_player_in_game(g, 'p0'));
  assert.throws(() => verify_player_in_game(g, 'ghost'), /not in game/);
  const pub = other_player(g.players[0]);
  assert.equal(pub.hand_length, 1, 'other_player exposes hand_length not the cards');
  assert.ok(!('hand' in pub), 'other_player hides the hand');
});

test('personalize_game returns a self view for a member, a public view otherwise', () => {
  const g = mkGame([mkPlayer(0, [C(0, 5), C(1, 6)]), mkPlayer(1, [C(2, 7)])]);
  const mine = personalize_game(g, 'p0') as PersonalGame;
  assert.ok(mine.self, 'member gets a self projection');
  assert.equal(mine.self.hand.length, 2, 'self keeps the real hand');
  assert.ok(mine.players.every(p => !('hand' in p)), 'other players are redacted');

  const spectator = personalize_game(g, 'observer');
  assert.ok(!('self' in spectator), 'non-member gets the public (no-self) view');
  assert.equal(spectator.players.length, 2, 'public view still lists players');
});

test('calculateEloChange is symmetric and zero at equal ratings', () => {
  assert.equal(calculateEloChange(1500, 1500, 1), 5, 'even matchup win = +K/2');
  assert.equal(calculateEloChange(1500, 1500, 0), -5, 'even matchup loss = -K/2');
  // A heavy favourite gains little for winning, loses a lot for losing.
  const favWin = calculateEloChange(2000, 1000, 1);
  const favLoss = calculateEloChange(2000, 1000, 0);
  assert.ok(favWin >= 0 && favWin < 5, 'favourite gains little on a win');
  assert.ok(favLoss < favWin, 'favourite loses more than it would gain');
});

test('calculateGameRankings: elimination order first, fool last, deduped', () => {
  const g = mkGame([mkPlayer(0, []), mkPlayer(1, []), mkPlayer(2, [])]);
  // p1 then p2 got out (with a duplicate to exercise the dedupe); p0 is the fool.
  g.elimination_order = ['p1', 'p2', 'p2'];
  const ranks = calculateGameRankings(g);
  assert.deepEqual(ranks, ['p1', 'p2', 'p0'], 'winners in elimination order, fool last, no dupes');
});

test('addLog stamps an id + created_at and appends to the game log', () => {
  const g = mkGame([mkPlayer(0, []), mkPlayer(1, [])]);
  addLog(g, { game_id: 'cu', log_type: LOG_TYPE.GOOD, player_id: 'p0', card_pairs: [], defender_index: null });
  assert.equal(g.logs.length, 1, 'log appended');
  assert.ok(g.logs[0].id && g.logs[0].created_at, 'id and created_at stamped');
  assert.equal(g.logs[0].log_type, LOG_TYPE.GOOD, 'log fields preserved');
});
