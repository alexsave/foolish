// Durable state codec parity — the packed `games.state bytea` format.
//
// serializeGameState/deserializeGameState (supabase/functions/_shared/wasm/
// engine.ts) pack a Game into the kernel's VERSIONED persist blob and read it
// back, reattaching the stable roster (ids/names/strategy) + the two
// presentation fields the kernel doesn't model (good_players order,
// good_timestamp value) from the row's roster columns. This is the format the
// server will persist instead of the scattered JSONB columns, so it must be a
// LOSSLESS round-trip on every reachable game state.
//
// The test plays thousands of seeded moves through the real kernel and, at
// every state, asserts:
//   1. deserialize(serialize(g)) reproduces g exactly (structural parity), and
//   2. serialize is deterministic and idempotent (byte-identical re-encode).
//
// Pure kernel test — needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { game_done } from '../supabase/functions/_shared/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { processBotAction, shouldBotActCore } from '../supabase/functions/_shared/pure_bot_actions.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';
import {
  serializeGameState, deserializeGameState, stateFormatVersion, RosterTemplate,
} from '../supabase/functions/_shared/wasm/engine.ts';
import {
  Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../supabase/functions/_shared/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const mkPlayer = (i: number): PrivatePlayer => ({
  player_id: `player-uuid-${i}-abcdef0123456789`, // full-width id (>24 chars): proves identity comes from the roster, not the blob
  name: `Player ${i}`, status: PLAYER_STATUS.READY, is_ai: true,
  hand: [], awaiting_attack: false, hand_length: 0, strategy_key: STRATEGY_KEY.RANDOM,
});
const mkGame = (np: number, id: string): Game => ({
  players: Array.from({ length: np }, (_, i) => mkPlayer(i)),
  deck: [], logs: [], id, name: `${id} game`, status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [], version: 7,
});

// The roster/identity + presentation columns the server stores alongside the blob.
const rosterOf = (g: Game): RosterTemplate => ({
  id: g.id, name: g.name, version: g.version, deck_length: g.deck.length,
  players: g.players.map(p => ({
    player_id: p.player_id, name: p.name, is_ai: p.is_ai, strategy_key: p.strategy_key,
  })),
  good_players: g.good_players, good_timestamp: g.good_timestamp,
});

// Everything the packed blob + roster must reproduce. Excludes purely-derived
// fields (hand_length/deck_length) and logs (persisted separately, stripped
// before any consumer sees the state).
const projection = (g: Game) => ({
  status: g.status, power_suit: g.power_suit, first_attacker: g.first_attacker,
  defender: g.defender, discard_pile_length: g.discard_pile_length, flipped: g.flipped,
  deck: g.deck, table_battles: g.table_battles, elimination_order: g.elimination_order,
  good_players: g.good_players, good_timestamp: g.good_timestamp,
  id: g.id, name: g.name, version: g.version,
  players: g.players.map(p => ({
    player_id: p.player_id, name: p.name, is_ai: p.is_ai, strategy_key: p.strategy_key,
    status: p.status, awaiting_attack: p.awaiting_attack, hand: p.hand,
  })),
});

const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');

test('kernel state codec reports a stable format version', () => {
  assert.equal(stateFormatVersion(), 1);
});

test('every reachable game state round-trips losslessly through the packed blob', async () => {
  let checks = 0;
  let maxBlob = 0;
  const VERSION = stateFormatVersion();

  for (const np of [2, 3, 4, 6]) {
    for (let seed = 0; seed < 40; seed++) {
      const g = mkGame(np, `s${np}-${seed}`);
      start_game(g);

      const roundTrip = () => {
        const blob = serializeGameState(g);
        assert.equal(blob[0], VERSION, 'blob carries the format-version byte');
        maxBlob = Math.max(maxBlob, blob.length);

        const restored = deserializeGameState(blob, rosterOf(g));
        // 1. structural parity: the restored game matches the original exactly.
        assert.deepEqual(projection(restored), projection(g),
          `round-trip mismatch at check ${checks}`);
        // 2. determinism/idempotence: re-encoding the restored game is byte-identical.
        assert.equal(hex(serializeGameState(restored)), hex(blob),
          `re-encode not byte-identical at check ${checks}`);
        checks++;
      };

      roundTrip(); // freshly-dealt state
      let guard = 0;
      while (game_done(g) === null && ++guard < 100000) {
        const eligible: PrivatePlayer[] = [];
        for (let i = 0; i < g.players.length; i++) {
          const p = g.players[i];
          if (shouldBotActCore(g, p, i) && calculateLegalMoves(g, p.player_id).length > 0) eligible.push(p);
        }
        if (eligible.length === 0) break;
        let acted = false;
        for (const p of eligible) { if (await processBotAction(g, p)) { acted = true; break; } }
        if (!acted) break;
        roundTrip(); // after every accepted move, incl. terminal states
      }
    }
  }

  assert.ok(checks > 5000, `expected thousands of round-trips, ran ${checks}`);
  // The blob must stay small — it's the whole point vs the JSONB columns.
  assert.ok(maxBlob < 2048, `packed state blob unexpectedly large: ${maxBlob} bytes`);
  console.error(`  state_codec: ${checks} round-trips, max blob ${maxBlob} bytes, v${VERSION}`);
});
