// The C rules kernel (c/src/game.c + legal.c, compiled to WASM) is the single
// source of truth for gameplay, and every server operation runs it through the C
// Table (c/src/table.h). This file guards the table's end of that:
//
//   1. the kernel obeys THE deck-size rule (2..5 players -> 36 cards,
//      6+ -> 52), settled once for every deployment (see c/src/card.h);
//   2. full random games through the table conserve cards and end with a
//      single fool at every player count;
//   3. hostile moves are refused with the engine's reason and never mutate the
//      stored game.
//
// The thin TS projections this file once held against the kernel (canCover,
// game_done, get_next_player_index, shouldBotActCore) are deleted with the TS
// game shape (plan Q11): the web asks the kernel directly (client_render_rules,
// client_guards, unambiguous_cover), so there is no second answer to compare.
//
// Pure kernel test - needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { encodeAction, AWIRE_KIND } from '../sdk/ts/wire/awire.ts';
import { fixtureTable, reasonOf } from './helpers/table_fixture.ts';
import { residentBoard, type BoardState, type PlayCard } from './helpers/table_play.ts';
import { botCycle, dealBotTable, seedBytes, type BotTableRow } from './helpers/bot_table.ts';
import { dealTable } from './helpers/kernel_board.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

const boardOf = (row: BotTableRow): BoardState => {
  assert.equal(fixtureTable().load(row.state, row.roster), L.TABLE_OK, 'the row loads');
  return residentBoard(row.gameId, row.state, row.roster);
};

const countCards = (b: BoardState): number =>
  b.deckCount + (b.trump ? 1 : 0) + b.discard
  + b.seats.reduce((a, s) => a + s.hand.length, 0)
  + b.battles.reduce((a, x) => a + 1 + (x.defense ? 1 : 0), 0);

// Aces are value 13 in the kernel's card numbering (card.h).
const ACE = 13;

test('kernel deals the settled deck size at every player count (6+ -> 52)', () => {
  for (let np = 2; np <= 8; np++) {
    for (let s = 0; s < 4; s++) {
      const b = boardOf(dealBotTable(Array(np).fill('random'), seedBytes(np, 900 + s)));
      const expected = np >= 6 ? 52 : 36;
      assert.equal(b.status, L.GAME_STATUS_PLAYING, `${np}p is dealt`);
      assert.equal(countCards(b), expected, `${np}p deals ${expected} cards`);
      assert.ok(b.trump === null || b.trump.value !== ACE, 'flipped trump is never an Ace');
    }
  }
});

test('kernel-driven random games conserve cards and end with one fool (2..8p)', () => {
  // Card conservation is the invariant asserted on EVERY committed state of every
  // game below. Termination is not a kernel property under RANDOM play: a small
  // share of random games reach a legal but non-terminating cycle (empty deck,
  // hands that can never cover one another, random defenders picking up forever).
  // That is random move choice, not the engine, so a finishing game is drawn by
  // RETRY, not by an ever-larger move cap.
  const CAP = 3000;
  const ATTEMPTS = 12;
  for (let np = 2; np <= 8; np++) {
    let finished = false;
    for (let attempt = 0; attempt < ATTEMPTS && !finished; attempt++) {
      let row = dealBotTable(Array(np).fill('random'), seedBytes(np, attempt * 131 + 7));
      const total = countCards(boardOf(row));
      for (let step = 0; step < CAP && row.status === L.GAME_STATUS_PLAYING; step++) {
        const c = botCycle(row, { maxActions: 1 });
        assert.ok(c.drive.n > 0, `${np}p: a bot moves while the game is playing`);
        row = c.row;
        assert.equal(countCards(boardOf(row)), total, `${np}p: card conservation at version ${row.version}`);
      }
      if (row.status === L.GAME_STATUS_GAME_OVER) {
        finished = true;
        const b = boardOf(row);
        assert.equal(b.eliminated.length, np - 1, 'everyone but the fool got out');
        assert.ok(row.fool >= 0 && !b.eliminated.includes(row.fool), 'the fool is the one seat not out');
      }
    }
    assert.ok(finished, `${np}p random game finishes within ${ATTEMPTS} deals`);
  }
});

test('hostile moves are refused with the engine reason and never mutate the game', () => {
  const seats = [0, 1, 2].map((i) => ({ id: `h${i}`, name: `H${i}` }));
  const row = dealTable(seats, seedBytes(3, 4242));
  const b = boardOf(row);
  const fa = b.firstAttacker;
  const attacker = b.seats[fa];
  const notMine: PlayCard = b.seats[(fa + 2) % 3].hand[0];
  const mine = attacker.hand[0];
  const table = fixtureTable();

  const attempt = (actorId: string, wire: Uint8Array) => {
    assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
    const rc = table.act(actorId, wire, null, 0);
    const detail = rc === L.TABLE_REJECTED ? table.reject() : 0;
    const p = table.commit(row.gameId, row.version + 1, 0);
    assert.ok(typeof p !== 'number');
    assert.equal(Buffer.from(p.state).toString('hex'), Buffer.from(row.state).toString('hex'),
      `a refused move (${reasonOf(rc, ['TABLE_'])}) left the stored game untouched`);
    return { rc, detail };
  };
  const rejected = (what: string, got: { rc: number; detail: number }, reason: number) => {
    assert.equal(got.rc, L.TABLE_REJECTED, `${what}: rejected, got ${reasonOf(got.rc, ['TABLE_'])}`);
    assert.equal(got.detail, reason, `${what}: ${reasonOf(reason, ['ENGINE_REJECT_'])}, got ${reasonOf(got.detail, ['ENGINE_REJECT_'])}`);
  };

  rejected('forged card', attempt(attacker.id, encodeAction({ kind: 'attack', cards: [notMine] })), L.ENGINE_REJECT_NOT_IN_HAND);
  rejected('duplicate', attempt(attacker.id, encodeAction({ kind: 'attack', cards: [mine, { ...mine }] })), L.ENGINE_REJECT_DUPLICATES);
  assert.equal(attempt('ghost', encodeAction({ kind: 'attack', cards: [mine] })).rc, L.TABLE_E_NOT_SEATED, 'non-member');
  assert.equal(attempt(attacker.id, Uint8Array.from([AWIRE_KIND.attack, 0xee])).rc, L.TABLE_E_WIRE, 'malformed payload');
  // And the move itself is legal: the refusals above are about what they changed.
  assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
  assert.equal(table.act(attacker.id, encodeAction({ kind: 'attack', cards: [mine] }), null, 0), L.TABLE_APPLIED, 'the honest attack applies');
});
