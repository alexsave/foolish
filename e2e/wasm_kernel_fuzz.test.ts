// Adversarial fuzz of the C/WASM kernel's DOORS.
//
// The kernel is freestanding C with no libc bounds checking: the state decoder
// reads counts (num_players, hand_count, deck_count, num_battles, ...) as loop
// bounds into fixed arrays, the legal-move enumerator indexes value-keyed stack
// arrays, the bot bitboards do `1 << card_id`, and the session-log importer walks
// records it did not write. Hostile bytes must NEVER crash the module, corrupt
// memory, or hang it (combinatorial move blow-up). The kernel refuses what it
// could not have produced (game_validate, the roster and wire checks) and defends
// itself regardless.
//
// Every game reaches the kernel through the C Table now, so this knocks on the
// table's doors with bytes rather than on the retired JS Game marshal: stored
// state and roster blobs (table_load), action wires (table_act), session logs
// (table_set_session_log) and the bot cycle over kernel-sealed boards
// (table_bot_drive), plus the client slot's envelope and push readers. A refusal
// code is an acceptable answer; a wasm trap or a hang is not. Pure kernel test -
// no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { clientTable } from '../sdk/ts/table/client_table.ts';
import { fixtureExports, fixtureTable, type TableFixture } from './helpers/table_fixture.ts';
import { dealBotTable, playBotTable, seedBytes } from './helpers/bot_table.ts';
import { lcg, randomPlayingBoard } from './helpers/kernel_board.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const CRASH = /table index|out of bounds|unreachable|memory access|RuntimeError/i;

const rnd = lcg(0xBADF00D);
const ri = (n: number) => Math.floor(rnd() * n);
const junk = (n: number) => Uint8Array.from({ length: n }, () => [0, 1, 2, 7, 13, 51, 52, 0x7f, 0x80, 0xfe, 0xff][ri(11)] ^ (rnd() < 0.3 ? ri(256) : 0));

/** Byte-level damage to a real blob: flips, truncation, extension. */
function damage(bytes: Uint8Array): Uint8Array {
  let out = bytes.slice();
  for (let i = 1 + ri(4); i > 0; i--) {
    switch (ri(4)) {
      case 0: if (out.length) out[ri(out.length)] = junk(1)[0]; break;
      case 1: if (out.length) out[ri(out.length)] ^= 1 << ri(8); break;
      case 2: out = out.slice(0, ri(out.length + 1)); break;
      case 3: { const e = new Uint8Array(out.length + 1 + ri(64)); e.set(out); e.set(junk(e.length - out.length), out.length); out = e; break; }
    }
  }
  return out;
}

/** Runs `fn`; a thrown JS error is an acceptable refusal, a wasm trap or a hang is not. */
function survives(what: string, fn: () => void, limitMs = 2000): void {
  const t0 = Date.now();
  try {
    fn();
  } catch (e) {
    if (e instanceof assert.AssertionError) throw e;
    if (process.env.E2E_VERBOSE) process.stderr.write(`[fuzz] ${what}: refused by a throw: ${String((e as Error)?.message ?? e).split('\n')[0]}\n`);
    const msg = String((e as Error)?.message ?? e);
    assert.ok(!CRASH.test(msg) && !(e instanceof WebAssembly.RuntimeError), `${what}: no wasm trap (${msg.split('\n')[0]})`);
  }
  assert.ok(Date.now() - t0 < limitMs, `${what}: no DoS hang (${Date.now() - t0} ms)`);
}

/** Everything a server does with a loaded table, on whatever did load. */
function exercise(what: string, gameId: string): void {
  const table = fixtureTable();
  const ex = fixtureExports();
  const seats = table.seats();
  for (let s = 0; s < seats.length + 2; s++) {
    const n = ex.wasm_legal_moves(s);
    assert.ok(n >= 0 && n <= 70000, `${what}: move count bounded for seat ${s} (${n})`);
  }
  for (const s of seats) {
    for (let k = 0; k < 3; k++) table.act(s.id, junk(ri(24)), null, 0);
  }
  table.setDealSeed('ab'.repeat(32));
  const d = table.botDrive(null, 2);
  assert.ok(typeof d === 'number' || d.n <= 2, `${what}: the drive stays within its cap`);
  const p = table.commit(gameId, 1, 0);
  if (typeof p !== 'number') {
    for (let v = -1; v < seats.length; v++) {
      table.push(gameId, v);
      const env = table.envelope(gameId, v, 1);
      if (env instanceof Uint8Array) clientTable().adoptEnvelope(env);
    }
  }
}

test('the load door survives damaged state and roster blobs (overflow / OOB / DoS classes)', () => {
  const rows: TableFixture[] = [];
  for (const np of [2, 3, 4, 6, 8]) rows.push(dealBotTable(Array(np).fill('random'), seedBytes(np, 31)));
  let loaded = 0, refused = 0;
  for (let iter = 0; iter < 1500; iter++) {
    const row = rows[ri(rows.length)];
    const state = rnd() < 0.8 ? damage(row.state) : row.state;
    const roster = rnd() < 0.3 ? damage(row.roster) : row.roster;
    survives(`iter ${iter}`, () => {
      const rc = fixtureTable().load(state, roster);
      if (rc !== L.TABLE_OK) { refused++; return; }
      loaded++;
      exercise(`iter ${iter}`, 'fuzz');
    });
  }
  assert.ok(refused > 500, `the kernel refused damaged blobs (${refused})`);
  assert.ok(loaded > 20, `some damaged blobs still loaded and were played on (${loaded})`);
});

test('the bot cycle survives hostile session logs on sealed boards (1<<card_id / belief build)', () => {
  // Every brain here is one bots.wasm links and the roster names. The logs are a
  // real finished game's, damaged: framing mostly intact, cards, seats and types not.
  const played = playBotTable(['random', 'random', 'random'], seedBytes(3, 77));
  const brains = ['random', 'firecracker', 'cordite', 'octogen', 'blackpowder'];
  let drives = 0, imported = 0;
  for (let iter = 0; iter < 60; iter++) {
    const brain = brains[iter % brains.length];
    const fx = randomPlayingBoard(rnd, () => brain, { battles: true });
    if (!fx) continue;
    const log = rnd() < 0.2 ? junk(ri(400)) : damage(played.log);
    survives(`${brain} iter ${iter}`, () => {
      const table = fixtureTable();
      assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK, 'a sealed board loads');
      table.setDealSeed('cd'.repeat(32));
      if (table.setSessionLog(log) >= 0) imported++;
      const d = table.botDrive(null, 1);
      if (typeof d !== 'number') drives++;
    }, 5000);
  }
  assert.ok(drives > 20 && imported > 0, `the bots drove on hostile logs (${drives} drives, ${imported} logs imported)`);
});

test('the action and client doors survive random bytes', () => {
  const row = dealBotTable(['random', 'random', 'random', 'random'], seedBytes(4, 5));
  const push = (() => {
    const t = fixtureTable();
    assert.equal(t.load(row.state, row.roster), L.TABLE_OK);
    const p = t.push('fuzz', -1);
    assert.ok(p instanceof Uint8Array);
    return p;
  })();
  for (let iter = 0; iter < 1500; iter++) {
    survives(`iter ${iter}`, () => {
      const table = fixtureTable();
      assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
      table.act(`seat-${ri(5)}`, junk(ri(40)), rnd() < 0.5 ? null : ri(5), ri(5));
      table.requestDecode(junk(ri(80)));
      clientTable().adoptEnvelope(rnd() < 0.5 ? junk(ri(300)) : damage(push));
      clientTable().readPush(damage(push), { as3: rnd() < 0.5, identity: rnd() < 0.5 ? 'none' : junk(ri(60)) });
    });
  }
});
