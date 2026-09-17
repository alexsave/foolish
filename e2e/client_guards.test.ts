// The client's move gates (src/utils/gameValidation.ts, over the kernel's
// client_validate on the board the screen holds) must answer with the EXACT
// verdict the authoritative server kernel gives - despite the board holding
// opponents' hands and the stock only as counts (the client can't see their
// cards). That equivalence holds because none of the validators inspect another
// player's card identity: a player's own move is judged only against their
// (real) hand, the public table, and opponents' COUNTS. This test proves it
// across random games, and measures call cost and memory flatness. The C twin
// is c/tests test_client_validate_is_the_engine.
//
// Pure kernel test - needs no Postgres. Phase 8 (docs/C_GAME_SHAPE_MIGRATION.md)
// moved it off the TypeScript Game: the authority is the C Table's table_act on
// the row, the client's board is the seat's envelope as the web reads it, and
// games are dealt from a seed and advanced by the kernel's bot cycle
// (e2e/helpers/table_mem.ts).

import { test } from 'node:test';
import assert from 'node:assert/strict';

import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import type { TableView, ViewCard as Card } from '../sdk/ts/table/client_table.ts';
import { canAttack, canPass, canPickup, canCoverPair, validateCover } from '../src/utils/gameValidation.ts';
import { __clientKernelExports, kernelUnambiguousCover } from '../sdk/ts/wasm/bots.ts';
import { encodeAction, type AwireMove } from '../sdk/ts/wire/awire.ts';
import { MemTable, residentMoves } from './helpers/table_mem.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const seedOf = (np: number) => Uint8Array.from({ length: 32 }, (_, i) => (i * 17 + np * 5 + 3) & 0xff);
const deal = (np: number): MemTable =>
  MemTable.deal(Array.from({ length: np }, (_, i) => ({ id: `p${i}`, name: `P${i}`, brain: 'random' })), seedOf(np), 'cg');

// Server-kernel oracle: the table applies a legal move and refuses an illegal one.
const legal = (t: MemTable, seat: number, move: AwireMove): boolean => {
  const { rc } = t.probe(seat, encodeAction(move));
  assert.ok(rc === L.TABLE_APPLIED || rc === L.TABLE_REJECTED, `a move is applied or rejected, got ${rc}`);
  return rc === L.TABLE_APPLIED;
};
const canCover = (view: TableView, covers: Card[], attacks: Card[]): boolean => {
  try { validateCover(view, covers, attacks); return true; } catch { return false; }
};

test('client gates == authoritative kernel across random games (2..6 players)', () => {
  let legalChecks = 0, illegalChecks = 0;

  for (let np = 2; np <= 6; np++) {
    const t = deal(np);
    let guard = 0;
    while (++guard < 300) {
      const g = t.board();
      if (g.status !== L.GAME_STATUS_PLAYING) break;
      for (let seat = 0; seat < np; seat++) {
        const p = g.seats[seat];
        if (p.status !== L.PLAYER_STATUS_IN) continue;
        const pg = t.view(seat);
        t.load();
        const moves = residentMoves(seat);

        // Every enumerated legal move must be accepted by the client gate too.
        for (const m of moves) {
          if (m.kind === 'attack') {
            assert.equal(canAttack(pg, m.cards), legal(t, seat, m), 'attack gate parity'); legalChecks++;
          } else if (m.kind === 'pass') {
            assert.equal(canPass(pg, m.cards), legal(t, seat, m), 'pass gate parity'); legalChecks++;
          } else if (m.kind === 'cover') {
            assert.equal(canCover(pg, m.cards, m.attack_cards!), legal(t, seat, m), 'cover gate parity'); legalChecks++;
          } else if (m.kind === 'pickup') {
            assert.equal(canPickup(pg), legal(t, seat, m), 'pickup gate parity'); legalChecks++;
          }
        }

        // Negatives: every card of the seat's hand as a lone attack and a lone
        // pass, and a card the seat does not hold (an opponent's). Both engines
        // must agree on each.
        for (const card of p.hand) {
          assert.equal(canAttack(pg, [card]), legal(t, seat, { kind: 'attack', cards: [card] }), 'lone attack parity');
          assert.equal(canPass(pg, [card]), legal(t, seat, { kind: 'pass', cards: [card] }), 'lone pass parity');
          illegalChecks += 2;
        }
        const foreign = g.seats[(seat + 1) % np].hand[0];
        if (foreign) {
          assert.equal(canAttack(pg, [foreign]), legal(t, seat, { kind: 'attack', cards: [foreign] }),
            'foreign-card attack parity'); illegalChecks++;
        }
        assert.equal(canPickup(pg), legal(t, seat, { kind: 'pickup' }), 'pickup parity');
      }

      if (t.drive(1).drive.n === 0) break;
    }
  }

  assert.ok(legalChecks > 300, `enough legal-move comparisons (${legalChecks})`);
  assert.ok(illegalChecks > 1000, `enough refused-move comparisons (${illegalChecks})`);
  console.error(`[client-guards] legal=${legalChecks} other=${illegalChecks}`);
});

test("canCoverPair is the cover rule, and the kernel's cover resolver agrees, over the full card cross-product", () => {
  for (let ps = 0; ps < 4; ps++) {
    for (let as = 0; as < 4; as++) for (let av = 1; av <= 13; av++) {
      for (let ds = 0; ds < 4; ds++) for (let dv = 1; dv <= 13; dv++) {
        const a: Card = { suit: as, value: av }, d: Card = { suit: ds, value: dv };
        const got = canCoverPair(a, d, ps);
        // The rule, stated: a higher card of the attack's suit, or any trump over a non-trump.
        const rule = (ds === as && dv > av) || (ds === ps && as !== ps);
        assert.equal(got, rule, `the cover rule: ${dv}/${ds} over ${av}/${as}, trump ${ps}`);
        const resolver = kernelUnambiguousCover([d], [{ attack: a, defense: null }], ps) !== null;
        assert.equal(got, resolver, "the kernel's cover resolver agrees");
      }
    }
  }
});

test('perf + mem: gates are fast and the module memory is flat (no leak)', () => {
  // A representative opening: the first attacker with an opener available.
  const t = deal(4);
  const seat = t.board().firstAttacker;
  const pg = t.view(seat);
  const card = pg.myHand[0];
  const memory = (__clientKernelExports() as unknown as { memory: WebAssembly.Memory }).memory;

  canAttack(pg, [card]);
  const memBefore = memory.buffer.byteLength;

  const N = 200_000;
  const t0 = performance.now();
  let truthy = 0;
  for (let i = 0; i < N; i++) { if (canAttack(pg, [card])) truthy++; }
  const dt = performance.now() - t0;

  const memAfter = memory.buffer.byteLength;
  const perCallUs = (dt / N) * 1000;
  console.error(`[client-guards] ${N} gate calls in ${dt.toFixed(0)}ms (${perCallUs.toFixed(2)}µs/call), truthy=${truthy}, mem=${(memBefore / 1024).toFixed(0)}KB`);

  assert.ok(truthy === N || truthy === 0, 'deterministic verdict across all calls');
  assert.ok(perCallUs < 25, `gate call is cheap (${perCallUs.toFixed(2)}µs, budget 25µs)`);
  assert.equal(memAfter, memBefore, 'wasm linear memory does not grow across 200k calls (no leak)');
});
