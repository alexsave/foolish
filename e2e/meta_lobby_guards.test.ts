// The meta handlers guard several actions to the WAITING lobby state (and cap
// the lobby at MAX_PLAYERS). The e2e meta suite drives real games that are
// always WAITING when these run, so the "wrong status" / "lobby full" throws
// go unexercised. handleMetaAction is a pure in-memory dispatch for these
// error paths, so this needs no Postgres.

// harness.ts (imported first) installs the Deno/EdgeRuntime globals that
// meta_actions -> utils -> auth reference at module load. No DB is touched.
import './harness.ts';
import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import { pgPool } from './harness.ts';

import { handleMetaAction } from '../supabase/functions/_shared/meta_actions.ts';
import { MAX_PLAYERS } from '../supabase/functions/_shared/constants.ts';
import {
  Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../supabase/functions/_shared/types.ts';
import type { ExecutionParams } from '../supabase/functions/_shared/utils.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const mkPlayer = (id: string, status = PLAYER_STATUS.IDLE): PrivatePlayer => ({
  player_id: id, name: id, status, is_ai: false, hand: [], awaiting_attack: false,
  hand_length: 0, strategy_key: STRATEGY_KEY.HUMAN,
});

const mkGame = (players: PrivatePlayer[], status = GAME_STATUS.WAITING): Game => ({
  players, deck: [], logs: [], id: 'g', name: 'g', status,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
});

const exec = (game: Game, userId: string, body: any): ExecutionParams => ({
  user: { id: userId, user_metadata: { username: userId } } as any,
  user_name: userId, body, game, reqId: 't',
});

test('join is rejected outside the WAITING lobby and when the lobby is full', async () => {
  const playing = mkGame([mkPlayer('h1')], GAME_STATUS.PLAYING);
  await assert.rejects(
    handleMetaAction(exec(playing, 'newbie', { type: 'join', game_id: 'g' })),
    /not waiting for players/i, 'join blocked on a live game',
  );

  const full = mkGame(Array.from({ length: MAX_PLAYERS }, (_, i) => mkPlayer(`h${i}`)));
  await assert.rejects(
    handleMetaAction(exec(full, 'newbie', { type: 'join', game_id: 'g' })),
    /full \(max/i, `join blocked at ${MAX_PLAYERS} players`,
  );
});

test('rearrange-players and update-name are lobby-only', async () => {
  const over = mkGame([mkPlayer('h1'), mkPlayer('h2')], GAME_STATUS.GAME_OVER);
  await assert.rejects(
    handleMetaAction(exec(over, 'h1', { type: 'rearrange-players', game_id: 'g', new_order: ['h2', 'h1'] })),
    /only rearrange players during game lobby/i, 'rearrange blocked once the game started',
  );

  const playing = mkGame([mkPlayer('h1')], GAME_STATUS.PLAYING);
  await assert.rejects(
    handleMetaAction(exec(playing, 'h1', { type: 'update-name', game_id: 'g', new_name: 'x' })),
    /only update name during game lobby/i, 'rename blocked once the game started',
  );
});

test('continue is rejected while the game is still in progress', async () => {
  const playing = mkGame([mkPlayer('h1', PLAYER_STATUS.IN)], GAME_STATUS.PLAYING);
  await assert.rejects(
    handleMetaAction(exec(playing, 'h1', { type: 'continue', game_id: 'g' })),
    /is not over/i, 'continue blocked mid-game',
  );
});

test('continue reset reports the winner, then the fool, then a bare reset', async () => {
  // A player already OUT is announced as the winner.
  const withWinner = mkGame([mkPlayer('w', PLAYER_STATUS.OUT), mkPlayer('f', PLAYER_STATUS.IN)], GAME_STATUS.GAME_OVER);
  const r1 = await handleMetaAction(exec(withWinner, 'w', { type: 'continue', game_id: 'g' }));
  assert.match(r1.events[0].message!, /won!/i, 'winner announced');
  assert.equal(r1.game.status, GAME_STATUS.WAITING, 'game returns to the lobby');
  assert.ok(r1.game.players.every(p => p.hand.length === 0), 'hands cleared on reset');

  // No winner but a lone IN player -> announced as the fool.
  const withFool = mkGame([mkPlayer('a', PLAYER_STATUS.IN), mkPlayer('b', PLAYER_STATUS.IDLE)], GAME_STATUS.GAME_OVER);
  const r2 = await handleMetaAction(exec(withFool, 'a', { type: 'continue', game_id: 'g' }));
  assert.match(r2.events[0].message!, /fool/i, 'fool announced when there is no OUT winner');
});

test('an unknown meta action type is rejected', async () => {
  const g = mkGame([mkPlayer('h1')]);
  await assert.rejects(
    handleMetaAction(exec(g, 'h1', { type: 'no-such-action' })),
    /unknown meta action type/i,
  );
});

// Release the harness's (unused) pg pool so the runner exits cleanly.
after(async () => { await pgPool.end(); });
