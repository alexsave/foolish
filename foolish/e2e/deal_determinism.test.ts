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
// have no native twin at whole-game length on purpose: whole octogen games cost
// more natively than the entire C suite, so per-cycle purity is proven in C
// (test_table_bot_drive_ignores_instance_history) and the whole game, and the
// withheld-seed divergence, only here.
//
// They were also this suite's longest file (31s of the e2e's ~58s wall on an
// 8-core Mac), for six whole octogen games where three questions need four: the
// seed-known game is played once per seed and shared by the replay and the
// withheld-seed tests, and the withheld game stops at the first cycle that
// differs, because the first difference is the whole of what that test asks.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { botCycle, dealBotTable } from './helpers/bot_table.ts';
import { hexOf, MemTable } from './helpers/table_mem.ts';

/**
 * The game `brain` plays from `seed` as the server's cycle chain commits it, one
 * line per committed cycle (who moved, and the durable state blob written),
 * opened by the deal and closed by the fool. With `until`, play stops at the
 * first cycle whose line is not that trace's: the game has already diverged.
 */
function playTrace(brain: string, seed: Uint8Array, id: string, opts: { withholdSeed?: boolean; until?: string[] } = {}): string[] {
  let row = dealBotTable([brain, brain], seed, { gameId: id });
  const dealt = new MemTable(row.state, row.roster, id).board();
  assert.equal(dealt.deterministic, true, 'seed-dealt game must set deterministic_deck');
  // Attacker model: an adversary who knows the public board (and thus the deck,
  // via pop-order) but NOT the server-only game_seed. Withhold it after the deal
  // so the deck is unchanged (deterministic_deck already set) but the bots seed
  // from base 0 instead of the real seed.
  if (opts.withholdSeed) row = { ...row, seedHex: '' };
  const trace = [`flip=${dealt.trump!.suit}.${dealt.trump!.value} fa=${dealt.firstAttacker}`];
  for (let guard = 0; guard < 4000 && row.status === L.GAME_STATUS_PLAYING; guard++) {
    const c = botCycle(row, { maxActions: 1 });
    if (c.drive.n === 0) break;
    row = c.row;
    trace.push(`${c.drive.seats.join(',')}:${hexOf(row.state)}`);
    if (opts.until && trace[trace.length - 1] !== opts.until[trace.length - 1]) return trace;
  }
  assert.equal(row.status, L.GAME_STATUS_GAME_OVER, `${id}: the game ended`);
  trace.push(`fool=${row.fool}`);
  return trace;
}

const digest = (trace: string[]): string => createHash('sha256').update(trace.join('\n')).digest('hex');

// A fixed seed (the reviewed replay's) plus a couple of arbitrary ones.
const SEEDS = [
  'da645ff515777b2c47d1c59937c7dbd637372ef1f2e440cf9867ea9cd2327d5f',
  '0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20',
].map(hex => Uint8Array.from(hex.match(/../g)!.map(b => parseInt(b, 16))));

// The seed-known octogen game of each seed, played once and asked twice.
const known = new Map<number, string[]>();
const octogenKnown = (si: number): string[] => {
  if (!known.has(si)) known.set(si, playTrace('octogen', SEEDS[si], `det-og-${si}`));
  return known.get(si)!;
};

for (const [si, seed] of SEEDS.entries()) {
  test(`simple_heuristic game replays identically from a pinned deal seed (#${si})`, () => {
    const a = digest(playTrace('simple_heuristic', seed, `det-sh-a-${si}`));
    const b = digest(playTrace('simple_heuristic', seed, `det-sh-b-${si}`));
    assert.equal(a, b, 'same deal seed must produce the same game (draws are deterministic)');
  });

  test(`octogen game replays identically from a pinned deal seed (#${si})`, () => {
    const again = playTrace('octogen', seed, `det-og-${si}`);
    assert.equal(digest(again), digest(octogenKnown(si)), 'octogen must be a pure function of the reproducible deal-seeded state');
  });
}

// SECURITY: octogen's play must depend on the SERVER-ONLY game_seed, so an
// attacker who can see the public board (and the deck via pop-order) but not the
// seed cannot reproduce/predict it. Same deal, seed known vs withheld: the
// games must diverge. If they matched, the bot RNG would be a pure function of
// public state and a source-code holder could predict every octogen move. The
// same game id on both sides, so the seed is the only thing that differs.
for (const [si, seed] of SEEDS.entries()) {
  test(`octogen is UNPREDICTABLE without the server-only game_seed (#${si})`, () => {
    const seen = octogenKnown(si);
    const withheld = playTrace('octogen', seed, `det-og-${si}`, { withholdSeed: true, until: seen });
    assert.notEqual(digest(withheld), digest(seen),
      'octogen played identically with the seed withheld - its RNG is predictable from public state');
  });
}

test('two DIFFERENT deal seeds produce different games (guard against a stuck deal)', () => {
  const a = digest(playTrace('simple_heuristic', SEEDS[0], 'det-diff-a'));
  const b = digest(playTrace('simple_heuristic', SEEDS[1], 'det-diff-b'));
  assert.notEqual(a, b);
});
