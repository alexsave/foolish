/* =============================================================================
 * The packed pipeline's own guarantees: no leaks, and a stream that decodes.
 * =============================================================================
 * Every move runs through the C Table (table_act) and the server broadcasts the
 * kernel's own per-viewer pushes (table_push). There is no second implementation
 * to compare against, so this asserts the kernel's output on its own terms:
 *   - the PERSONALIZATION invariant: a viewer's stream never carries another
 *     player's hand identities, and DEAL/REFILL identities reach only the
 *     receiving seat (assertNoLeaks, through the client's reader);
 *   - a COVER event's target card and battle index agree - the two adjacent
 *     optional bytes a reader could take in either order;
 *   - the stream DECODES, and the board it decodes to mirrors the committed state;
 *   - the session log the commits append reads back whole (table_import_session_log)
 *     and, at the game end, encodes a verified replay code (table_replay_code);
 *   - an illegal wire is rejected.
 *
 * The TS log decoder this file also used (logwire decodeLogs) is retired with the
 * TS game shape: the log is read by the kernel only.
 *
 * Pure kernel test - needs no Postgres (runs under VALIDATION_ONLY too).
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import { clientTable } from '../sdk/ts/table/client_table.ts';
import { EVW_T_COVER, EVW_T_DEAL, EVW_T_REFILL } from '../sdk/ts/gen/view_layout.bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { legalMoves, residentBoard, type BoardState } from './helpers/table_play.ts';
import { dealTable, lcg } from './helpers/kernel_board.ts';
import { seedBytes, type BotTableRow } from './helpers/bot_table.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// Deterministic RNG (same LCG as the fuzz suite) so failures reproduce.
const rnd = lcg(Number(process.env.FUZZ_SEED || 0xbadc0de5));
const ri = (n: number) => Math.floor(rnd() * n);

// Humans at even seats, bots at odd ones.
const seatsFor = (np: number) => Array.from({ length: np }, (_, i) => ({ id: `player-${i}`, name: `P${i}`, brain: i % 2 === 1 ? 'random' : '' }));

function boardOf(row: BotTableRow): BoardState {
  assert.equal(fixtureTable().load(row.state, row.roster), L.TABLE_OK, 'the row loads');
  return residentBoard(row.gameId, row.state, row.roster);
}

// Personalization scan: every step of a viewer's stream shows no hand but the
// viewer's own, and DEAL/REFILL card identities reach only the receiving seat.
//
// It asks the client's reader (the kernel's client slot), which is the honest way
// round: it inspects what a CLIENT can see, and a view the slot reads holds the
// viewer's own hand and every other seat as a count only - no other hand is there
// to be read.
function assertNoLeaks(bytes: Uint8Array, viewer: number) {
  assert.equal(bytes[0], 1, 'evwire format version');
  const read = clientTable().readPush(bytes, { as3: true, identity: 'none' });
  assert.ok(read, 'the stream reads');
  assert.equal(read.final.mySeat, viewer, 'the stream is addressed to this viewer');

  const ownHandOnly = (view: typeof read.final, where: string) => {
    assert.equal(view.mySeat, viewer, `the view is the viewer's (${where})`);
    assert.equal(view.myHand.length, viewer < 0 ? 0 : view.seats[viewer].handCount, `only the viewer's own hand (${where})`);
  };

  let checked = 0, covers = 0;
  for (const { event: ev, view } of read.steps) {
    // A card bound for someone else's hand.
    if ((ev.type === EVW_T_DEAL || ev.type === EVW_T_REFILL) && ev.seat !== viewer) {
      for (const c of ev.cards) {
        assert.deepEqual(c, { suit: -1, value: -1 }, `deal/refill cards masked (viewer ${viewer}, seat ${ev.seat})`);
      }
    }
    ownHandOnly(view, `event ${ev.type}`);

    // A COVER is the one event that carries BOTH optional trailer fields - the
    // covered attack card and the battle index it landed in (evwire.c's
    // ENGINE_HOOK_COVER emit). They are two adjacent single bytes behind one
    // flags byte, so a reader that takes them in the wrong order still decodes
    // the whole stream and still masks every hand: the leak scan above cannot
    // see it, and neither can a shape check. Tying them together is what makes
    // the order observable - the target must be the attack card sitting in the
    // battle the event names.
    if (ev.type === EVW_T_COVER) {
      assert.ok(ev.hasTarget, 'a cover names the card it covered');
      assert.ok(ev.battle >= 0, 'a cover names the battle it landed in');
      const bt = view.battles[ev.battle];
      assert.ok(bt, `cover battle index ${ev.battle} is on the board (${view.battles.length} battles)`);
      assert.deepEqual(bt.attack, ev.target,
        `cover target ${JSON.stringify(ev.target)} is battle ${ev.battle}'s attack ${JSON.stringify(bt.attack)}`);
      covers++;
    }
    checked++;
  }
  ownHandOnly(read.final, 'trailer');
  assert.equal(checked, read.steps.length, 'every event was scanned');
  return { covers, final: read.final };
}

test('every packed stream is leak-free, decodable, and mirrors the committed state', () => {
  const GAMES = Number(process.env.PARITY_GAMES || 24);
  const table = fixtureTable();
  let moves = 0, ends = 0, coversChecked = 0, codes = 0;

  for (let g = 0; g < GAMES; g++) {
    const np = g % 7 === 6 ? 6 : 2 + (g % 4); // 2..5 players (36-card deck), and 6
    let row = dealTable(seatsFor(np), seedBytes(np, g * 7919 + 13), { gameId: 'parity' });
    const humanSeats = seatsFor(np).flatMap((s, i) => (s.brain ? [] : [i]));

    for (let mv = 0; mv < 600 && row.status === L.GAME_STATUS_PLAYING; mv++) {
      const menu = legalMoves(boardOf(row));
      if (menu.length === 0) break;
      const m = menu[ri(menu.length)];

      assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
      assert.equal(table.setDealSeed(row.seedHex), L.TABLE_OK);
      assert.equal(table.act(m.playerId, m.wire, null, 0), L.TABLE_APPLIED, `the packed path applied (${m.kind})`);
      const p = table.commit(row.gameId, row.version + 1, 1_700_000_000_000 + mv);
      assert.ok(typeof p !== 'number', `commit products (${p})`);
      const pushes = [...humanSeats, -1].map((viewer) => {
        const bytes = table.push(row.gameId, viewer);
        assert.ok(bytes instanceof Uint8Array, `push for viewer ${viewer} (${bytes})`);
        return { viewer, bytes };
      });
      const log = p.logs === null ? row.log : p.logsReset ? p.logs : new Uint8Array(Buffer.concat([row.log, p.logs]));
      row = { ...row, version: row.version + 1, state: p.state, roster: p.roster, log, status: p.status, fool: p.fool };

      // The session log the commits append is one the kernel reads back whole.
      assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
      assert.ok(table.importSessionLog(row.log) > 0, 'the session log reads back');

      const b = boardOf(row);
      for (const { viewer, bytes } of pushes) {
        const { covers, final } = assertNoLeaks(bytes, viewer);
        coversChecked += covers;
        // The decoded final board mirrors the committed public state.
        assert.equal(final.status, b.status, 'status');
        assert.equal(final.deckCount, b.deckCount, 'deck length');
        assert.equal(final.discardPileLength, b.discard, 'discard length');
        assert.equal(final.goodMask, b.goodMask, 'goods said');
      }
      moves++;
      if (p.ended) {
        ends++;
        assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
        const code = table.replayCode(seedBytes(np, g * 7919 + 13), row.log);
        assert.ok(code instanceof Uint8Array, `the finished game's session log encodes a verified replay code (${code})`);
        codes++;
      }
    }
  }

  assert.ok(moves > 100, `enough moves exercised (${moves})`);
  // A cover is the only event with both trailer bytes, so the check above is
  // only worth anything if covers actually happened.
  assert.ok(coversChecked > 100, `enough covers cross-checked (${coversChecked})`);
  assert.ok(codes > 0, `some games ended and encoded (${codes})`);
  console.error(`[packed-wire] moves=${moves} ends=${ends} covers=${coversChecked}`);

  // An illegal wire is rejected: the defender may not attack.
  {
    const row = dealTable(seatsFor(3), seedBytes(3, 99), { gameId: 'parity' });
    const b = boardOf(row);
    const defender = b.seats[b.defender];
    assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
    const rc = table.act(defender.id, encodeAction({ kind: 'attack', cards: [defender.hand[0]] }), null, 0);
    assert.equal(rc, L.TABLE_REJECTED, 'the defender attacking is rejected by the kernel');
  }
});
