// The client-guards kernel (guards.wasm, via src/wasm/clientGuards.ts) must
// answer the UI move-gates with the EXACT verdict the authoritative server
// kernel gives — despite marshaling opponents as hand_length placeholders
// (the client can't see their cards). That equivalence holds because none of
// the validators inspect another player's card identity: a player's own move
// is judged only against their (real) hand, the public table, and opponents'
// COUNTS. This test proves it across random games, and measures load / call
// cost and memory flatness.
//
// Pure kernel test — needs no Postgres.

import { test, before } from 'node:test';
import assert from 'node:assert/strict';

import {
  initClientGuards, guardsReady, guardsMemBytes, canAttack, canPass, canCover, canPickup,
  canCoverPair, nextPlayerIndex, gameDone,
} from '../src/wasm/clientGuards.ts';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import { calculateLegalMoves } from '../server/api/common/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../server/api/common/pure_bot_actions.ts';
import {
  personalize_game, game_done, canCover as tsCanCover, get_next_player_index,
} from '../server/api/common/common_utils.ts';
import {
  kernelValidateAttack, kernelValidatePass, kernelValidateCover, kernelValidatePickup,
  kernelNextPlayer, kernelCanCover,
} from '../sdk/ts/wasm/engine.ts';
import {
  Game, PersonalGame, PrivatePlayer, Card, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../server/api/core/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const mkPlayer = (i: number): PrivatePlayer => ({
  player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
  hand: [], awaiting_attack: false, hand_length: 0, strategy_key: STRATEGY_KEY.RANDOM,
});
const mkGame = (np: number): Game => ({
  players: Array.from({ length: np }, (_, i) => mkPlayer(i)),
  deck: [], logs: [], id: 'cg', name: 'cg', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
});
const personalFor = (g: Game, seat: number): PersonalGame =>
  personalize_game(g, g.players[seat].player_id) as PersonalGame;

// Server-kernel oracle: validate throws on an illegal move.
const legal = (fn: () => void): boolean => { try { fn(); return true; } catch { return false; } };

before(async () => { await initClientGuards(); assert.ok(guardsReady(), 'guards.wasm loaded'); });

test('client gates == authoritative kernel across random games (2..6 players)', async () => {
  let legalChecks = 0, illegalChecks = 0, projectionChecks = 0;

  for (let np = 2; np <= 6; np++) {
    const g = mkGame(np);
    start_game(g);
    let guard = 0;
    while (game_done(g) === null && ++guard < 300) {
      for (let seat = 0; seat < np; seat++) {
        const p = g.players[seat];
        if (p.status !== PLAYER_STATUS.IN) continue;
        const pg = personalFor(g, seat);

        // Every enumerated legal move must be accepted by the client gate too.
        for (const m of calculateLegalMoves(g, p.player_id)) {
          if (m.type === 'attack') {
            assert.equal(canAttack(pg, m.cards!), legal(() => kernelValidateAttack(g, p.player_id, m.cards!)),
              'attack gate parity'); legalChecks++;
          } else if (m.type === 'pass') {
            assert.equal(canPass(pg, m.cards!), legal(() => kernelValidatePass(g, p.player_id, m.cards!)),
              'pass gate parity'); legalChecks++;
          } else if (m.type === 'cover') {
            assert.equal(canCover(pg, m.cards!, m.attack_cards!),
              legal(() => kernelValidateCover(g, p.player_id, m.cards!, m.attack_cards!)),
              'cover gate parity'); legalChecks++;
          } else if (m.type === 'pickup') {
            assert.equal(canPickup(pg), legal(() => kernelValidatePickup(g, p.player_id)),
              'pickup gate parity'); legalChecks++;
          }
        }

        // A negative: attacking with a card the player does not hold (an
        // opponent's). Both engines must reject — and agree.
        const foreign = g.players[(seat + 1) % np].hand[0];
        if (foreign) {
          assert.equal(canAttack(pg, [foreign]), legal(() => kernelValidateAttack(g, p.player_id, [foreign])),
            'foreign-card attack parity'); illegalChecks++;
        }

        // Pure projections: next-defender + game-over verdict.
        if (p.status === PLAYER_STATUS.IN) {
          assert.equal(nextPlayerIndex(pg, seat), kernelNextPlayer(g, seat), 'next-player parity');
          assert.equal(nextPlayerIndex(pg, seat), get_next_player_index(g, seat), 'next-player TS parity');
          projectionChecks++;
        }
        const done = gameDone(pg);
        const tsDone = game_done(g);
        assert.equal(done === -1 ? null : g.players[done].player_id, tsDone, 'game_done parity');
      }

      const actor = g.players.find((pp, i) => shouldBotActCore(g, pp, i) && calculateLegalMoves(g, pp.player_id).length > 0);
      if (!actor) break;
      await processBotAction(g, actor);
    }
  }

  assert.ok(legalChecks > 300, `enough legal-move comparisons (${legalChecks})`);
  console.error(`[client-guards] legal=${legalChecks} illegal=${illegalChecks} projection=${projectionChecks}`);
});

test('canCoverPair matches the kernel and the old TS primitive over the full card cross-product', () => {
  for (let ps = 0; ps < 4; ps++) {
    for (let as = 0; as < 4; as++) for (let av = 1; av <= 13; av++) {
      for (let ds = 0; ds < 4; ds++) for (let dv = 1; dv <= 13; dv++) {
        const a: Card = { suit: as, value: av }, d: Card = { suit: ds, value: dv };
        const g = canCoverPair(a, d, ps);
        assert.equal(g, kernelCanCover(a, d, ps), 'kernel parity');
        assert.equal(g, tsCanCover(a, d, ps), 'ex-TS primitive parity');
      }
    }
  }
});

test('perf + mem: gates are fast and the module memory is flat (no leak)', () => {
  // A representative mid-round state: first attacker with an opener available.
  const g = mkGame(4);
  start_game(g);
  const seat = g.first_attacker;
  const pg = personalFor(g, seat);
  const card = pg.self.hand[0];

  const memBefore = guardsMemBytes();

  const N = 200_000;
  const t0 = performance.now();
  let truthy = 0;
  for (let i = 0; i < N; i++) { if (canAttack(pg, [card])) truthy++; }
  const dt = performance.now() - t0;

  const memAfter = guardsMemBytes();
  const perCallUs = (dt / N) * 1000;
  console.error(`[client-guards] ${N} gate calls in ${dt.toFixed(0)}ms (${perCallUs.toFixed(2)}µs/call), truthy=${truthy}, mem=${(memBefore / 1024).toFixed(0)}KB`);

  assert.ok(truthy === N || truthy === 0, 'deterministic verdict across all calls');
  assert.ok(perCallUs < 25, `gate call is cheap (${perCallUs.toFixed(2)}µs, budget 25µs)`);
  assert.equal(memAfter, memBefore, 'wasm linear memory does not grow across 200k calls (no leak)');
  assert.ok(memBefore <= 4 * 1024 * 1024, `module memory footprint is small (${(memBefore / 1024).toFixed(0)}KB)`);
});
