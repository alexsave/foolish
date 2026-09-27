// Determinism guard for the seed-dealt game: with a PINNED 32-byte deal seed,
// a whole game must replay byte-for-byte through the bot cycle - the path a
// bot-vs-bot game actually takes on the server (bot_actions.ts runCycle: load the
// row, set its deal seed, table_bot_drive, commit). This is the regression guard
// for the bug where the state import dropped the deterministic-deck flag, so
// mid-game refills drew a RANDOM card and the game stopped being reproducible
// from its deal seed after the opening.
//
// simple_heuristic is the workhorse: it consumes NO RNG and keeps NO per-game
// memory, so any divergence is the DECK (draws), not the bot. octogen (which
// samples worlds seeded from game state) is checked too - it must be a pure
// function of the reproducible state.
//
// Phase 8 (docs/C_GAME_SHAPE_MIGRATION.md) moved this off the TypeScript Game
// (start_game_packed, processBotActionPacked, game_done) onto the C Table
// (e2e/helpers/bot_table.ts): one action per committed cycle, each hashed as the
// durable state blob it wrote.
//
// The native twin of the simple_heuristic cases is c/tests/tests.c
// test_table_bots_only_game_replays_from_its_seed: the same two seeds, the same
// cycle chain through the C Table, whole games. Those cases cost about a
// millisecond here and stay as the wasm build's run of it. The octogen cases
// have no native twin at whole-game length on purpose: six whole octogen games
// cost more natively than the entire C suite, so per-cycle purity is proven in
// C (test_table_bot_drive_ignores_instance_history) and the whole game, and the
// withheld-seed divergence, only here.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { botCycle, dealBotTable } from './helpers/bot_table.ts';
import { hexOf, MemTable } from './helpers/table_mem.ts';

function playHash(brain: string, seed: Uint8Array, id: string, withholdSeed = false): string {
  let row = dealBotTable([brain, brain], seed, { gameId: id });
  const dealt = new MemTable(row.state, row.roster, id).board();
  assert.equal(dealt.deterministic, true, 'seed-dealt game must set deterministic_deck');
  // Attacker model: an adversary who knows the public board (and thus the deck,
  // via pop-order) but NOT the server-only game_seed. Withhold it after the deal
  // so the deck is unchanged (deterministic_deck already set) but the bots seed
  // from base 0 instead of the real seed.
  if (withholdSeed) row = { ...row, seedHex: '' };
  const h = createHash('sha256');
  h.update(`flip=${dealt.trump!.suit}.${dealt.trump!.value} fa=${dealt.firstAttacker}
`);
  for (let guard = 0; guard < 4000 && row.status === L.GAME_STATUS_PLAYING; guard++) {
    const c = botCycle(row, { maxActions: 1 });
    if (c.drive.n === 0) break;
    row = c.row;
    h.update(`${c.drive.seats.join(',')}:${hexOf(row.state)}
`);
  }
  assert.equal(row.status, L.GAME_STATUS_GAME_OVER, `${id}: the game ended`);
  h.update(`fool=${row.fool}
`);
  return h.digest('hex');
}

// A fixed seed (the reviewed replay's) plus a couple of arbitrary ones.
const SEEDS = [
  'da645ff515777b2c47d1c59937c7dbd637372ef1f2e440cf9867ea9cd2327d5f',
  '0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20',
].map(hex => Uint8Array.from(hex.match(/../g)!.map(b => parseInt(b, 16))));

for (const [si, seed] of SEEDS.entries()) {
  test(`simple_heuristic game replays identically from a pinned deal seed (#${si})`, () => {
    const a = playHash('simple_heuristic', seed, `det-sh-a-${si}`);
    const b = playHash('simple_heuristic', seed, `det-sh-b-${si}`);
    assert.equal(a, b, 'same deal seed must produce the same game (draws are deterministic)');
  });

  test(`octogen game replays identically from a pinned deal seed (#${si})`, () => {
    const a = playHash('octogen', seed, `det-og-a-${si}`);
    const b = playHash('octogen', seed, `det-og-b-${si}`);
    assert.equal(a, b, 'octogen must be a pure function of the reproducible deal-seeded state');
  });
}

// SECURITY: octogen's play must depend on the SERVER-ONLY game_seed, so an
// attacker who can see the public board (and the deck via pop-order) but not the
// seed cannot reproduce/predict it. Same deal, seed known vs withheld: the
// games must diverge. If they matched, the bot RNG would be a pure function of
// public state and a source-code holder could predict every octogen move.
for (const [si, seed] of SEEDS.entries()) {
  test(`octogen is UNPREDICTABLE without the server-only game_seed (#${si})`, () => {
    const known    = playHash('octogen', seed, `sec-known-${si}`, false);
    const withheld = playHash('octogen', seed, `sec-withheld-${si}`, true);
    assert.notEqual(known, withheld,
      'octogen played identically with the seed withheld - its RNG is predictable from public state');
  });
}

test('two DIFFERENT deal seeds produce different games (guard against a stuck deal)', () => {
  const a = playHash('simple_heuristic', SEEDS[0], 'det-diff-a');
  const b = playHash('simple_heuristic', SEEDS[1], 'det-diff-b');
  assert.notEqual(a, b);
});
