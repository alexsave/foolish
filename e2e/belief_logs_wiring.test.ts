// Wiring-level integration test for the belief-bot session log - the layer the
// unit test (belief_logs.test.ts) can't reach.
//
// belief_logs.test.ts pins the KERNEL CONTRACT (importLogs honors belief_logs).
// But the bug that actually shipped was in the WIRING: the production bot loop
// loaded state without the session log and, for a long window, never
// repopulated it - so the belief bots chose blind. No test caught it because
// the offline arena/eval runs in-memory self-play where the log accumulates
// naturally; nothing exercised the real DB-reload path.
//
// This test closes that gap on the C Table (docs/C_GAME_SHAPE_MIGRATION.md
// Phase 4b). It drives the REAL server bot loop (bot_actions.ts lockedBotLoop:
// load the row, the kernel's table_bots_need_logs, the games.logs_packed read,
// the drive, commit_table) against a REAL Postgres via the harness, and asserts
// that octogen actually SEES the accumulating session log at choose time.
//
// It observes the two halves at the two places they are true:
//
//   - WHAT THE BOT SAW comes from the kernel (the belief probe), which records
//     the log as the strategy was about to read it, on the server's table
//     instance the loop's drive runs on. A spy here could only prove the bytes
//     were HANDED OVER, never that the importer spliced them into the board
//     octogen read - and that gap is where "octogen chose blind" lived.
//   - THE FED BYTES stay observed here, because the resident log's arithmetic
//     (read once, then concat-and-carry across cycles of a bots-only table) is
//     this side's job, not the kernel's. They are captured at the table calls
//     the loop makes (ServerTable.importSessionLog, then botDrive), on exactly
//     the cycles the kernel said a belief bot was about to choose.

import './harness.ts'; // sets Deno globals BEFORE any server module loads
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, uuid } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { decodeLogs } from '../sdk/ts/wire/logwire.ts';
import { bytesToBareHex } from '../sdk/ts/wire/bytes.ts';
import { ServerTable, serverTable } from '../sdk/ts/table/server_table.ts';
import { parseBeliefProbe } from '../sdk/ts/wasm/bots.ts';
import { mustReadTable } from './helpers/table_play.ts';
import { fixture } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// Bot pacing collapsed, as e2e/helpers/edge.ts does it: the loop's wait between
// cycles is a presentation delay, not CPU (it cost ~7 s a run). It does change
// how long the CPU predictor lets a segment run (a warm search is cheaper), so
// the segment is bounded below by a lease takeover instead, deterministically.
const realSetTimeout = globalThis.setTimeout;
(globalThis as { setTimeout: unknown }).setTimeout = ((fn: (...a: unknown[]) => void, ms?: number, ...args: unknown[]) =>
  realSetTimeout(fn, (ms ?? 0) >= 250 && (ms ?? 0) <= 5000 ? 0 : ms, ...args)) as unknown as typeof setTimeout;

// The session-log bytes the loop hands the kernel, per belief cycle.
// BARE hex (no \x) - matches how logs_packed is stored, so a prefix compare works.
const fed: string[] = [];

// Committed cycles in the one drive segment: enough for the log to grow well past
// empty and for the resident log to be carried several times.
const SEGMENT_CYCLES = 16;

// Watch the table calls the loop makes, without touching production code: the
// session log it hands the kernel (importSessionLog) and, at each drive, whether
// the kernel said a belief bot was about to choose. A load starts a cycle's
// section with nothing imported, so a belief cycle that imported nothing records
// an empty buffer and fails the non-empty check below.
function watchTable(): void {
  const proto = ServerTable.prototype as unknown as {
    load: (...a: unknown[]) => number; importSessionLog: (log: Uint8Array) => number; botDrive: (...a: unknown[]) => unknown;
    botsNeedLogs: () => boolean;
  };
  let imported = '';
  const load = proto.load, importLog = proto.importSessionLog, drive = proto.botDrive;
  proto.load = function (this: unknown, ...a: unknown[]) { imported = ''; return load.apply(this, a); };
  proto.importSessionLog = function (this: unknown, log: Uint8Array) { imported = bytesToBareHex(log).toLowerCase(); return importLog.call(this, log); };
  proto.botDrive = function (this: { botsNeedLogs: () => boolean }, ...a: unknown[]) {
    if (this.botsNeedLogs()) fed.push(imported);
    return drive.apply(this, a);
  };
}

// The server modules, with the segment bounded (tsx transforms these files to
// CJS, where top-level await is unavailable, hence the dynamic imports).
async function wireLoop() {
  watchTable();
  // End the segment after SEGMENT_CYCLES commits: the next lease renewal
  // reports that another loop took the lease over, which is how a real segment
  // yields. One segment must not finish the game (the end retires logs_packed,
  // the prefix check's reference), and the CPU predictor alone cannot promise it.
  const { supabaseClient } = await import('../server/impls/supabase/functions/_shared/adapter/utils.ts');
  const client = supabaseClient as unknown as { rpc: (name: string, params?: Record<string, unknown>) => Promise<unknown> };
  const rpc = client.rpc;
  let commits = 0;
  client.rpc = async (name, params) => {
    if (name === 'commit_table') commits++;
    if (name === 'renew_bot_lease' && commits >= SEGMENT_CYCLES) return { data: false, error: null };
    return rpc.call(client, name, params);
  };
  const { lockedBotLoop } = await import('../server/impls/supabase/functions/_shared/adapter/bot_actions.ts');
  const { runMeta } = await import('./helpers/table_server.ts');
  const { __setTableDealSeedOverride } = await import('../server/impls/supabase/functions/_shared/adapter/table_io.ts');
  const table = await serverTable();
  return {
    lockedBotLoop, runMeta, __setTableDealSeedOverride,
    wasmBeliefProbeReset: () => table.__beliefProbeReset(),
    wasmBeliefProbeDump: () => { const d = table.__beliefProbeDump(); return parseBeliefProbe(d.bytes, d.n); },
  };
}

let restoreSeed: (() => void) | null = null;
before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });
after(() => { restoreSeed?.(); });

// Number of session-log records currently persisted for this game (the source
// of truth a belief bot is supposed to see), through the real decoder.
async function persistedLogCount(gameId: string, seatIds: string[]): Promise<number> {
  const hex = (await mustReadTable(gameId)).logsPacked;
  if (!hex) return 0;
  try { return decodeLogs(Buffer.from(hex.replace(/^\\x/, ''), 'hex'), gameId, seatIds.map((player_id) => ({ player_id }))).length; }
  catch { return -1; }
}

test('the server bot loop feeds octogen the whole session log (not an empty one)', async () => {
  const { lockedBotLoop, runMeta, __setTableDealSeedOverride, wasmBeliefProbeReset, wasmBeliefProbeDump } = await wireLoop();
  // A pinned deal: the bots' searches are seeded from it, so the run is reproducible.
  __setTableDealSeedOverride(Uint8Array.from({ length: 32 }, (_, i) => (i * 53 + 0x0c) & 0xff));
  restoreSeed = () => __setTableDealSeedOverride(null);

  const gameId = `bw${uuid().slice(0, 6)}`;
  // Bots-only so lockedBotLoop drives the whole thing and carries its resident
  // log. Both octogen: the bug is about octogen's own memory, and self-play keeps
  // the belief demand high.
  const seats = [
    { id: uuid(), name: 'Octo0', brain: 'octogen' },
    { id: uuid(), name: 'Octo1', brain: 'octogen' },
  ];
  await seedTable(gameId, fixture().seats(seats).build());
  // The deal: a lobby ready sent as a seated bot's id. No client can send it (a
  // bot has no auth account), but it is the kernel's own deal through the real
  // meta handler and commit_table, which writes the deal's log records and seed.
  await runMeta(gameId, seats[0].id, { type: 'start' });
  const dealt = await mustReadTable(gameId);
  assert.equal(dealt.status, L.GAME_STATUS_PLAYING, 'fixture: the bots-only table dealt');
  assert.ok(dealt.needsBotsColumn, 'fixture: the kernel says the bots have work');

  // One drive segment, at most SEGMENT_CYCLES commits long. Octogen is heavy, so
  // the CPU predictor may bail sooner - that's fine: a handful is enough for the
  // session log to grow past empty and prove the wiring delivers it.
  fed.length = 0;
  wasmBeliefProbeReset();
  await lockedBotLoop(gameId);
  // Every search the kernel ran, with the log as the strategy was about to read it.
  const searches = wasmBeliefProbeDump();

  const final = await mustReadTable(gameId);
  assert.equal(final.status, L.GAME_STATUS_PLAYING, 'precondition: one segment does not finish the game (a finished game retires its log)');
  const sessionLen = await persistedLogCount(gameId, seats.map((s) => s.id));
  const maxSeen = Math.max(0, ...searches.map((s) => s.nLogs));
  process.stdout.write(`[wiring] octogen searches=${searches.length} maxKernelLogs=${maxSeen} fedBuffers=${fed.length} persistedSessionLen=${sessionLen}\n`);

  // octogen actually got to choose through the real loop.
  assert.ok(searches.length > 0, 'octogen never chose through the real bot loop');
  // The loop hydrated on every cycle a belief bot was eligible.
  assert.ok(fed.length > 0 && fed.every((h) => h.length > 0),
    'the session log was empty at a belief-bot drive - hydration wiring missing');

  // THE REGRESSION GUARD: once the session has accumulated records, octogen must
  // see them. Pre-fix, the log the chooser saw was pinned at 0 no matter how long
  // the game ran. The game produces several log records per bout (attack/cover/
  // draw/discard), so a non-trivial max proves the whole session reaches the
  // kernel - asserted of the board the strategy read, not of the bytes handed over.
  assert.ok(sessionLen >= 4, `precondition: the drive should persist a real session (got ${sessionLen})`);
  assert.ok(maxSeen >= 4,
    `octogen searched with a near-empty log (max=${maxSeen}) while ${sessionLen} records were persisted - belief bot is running blind`);

  // RESIDENT-LOG CORRECTNESS: the loop carried the log across cycles and appended
  // each committed cycle's bytes instead of re-reading the DB. Every buffer it fed
  // the kernel must therefore be a byte-exact PREFIX of the final persisted
  // logs_packed - if the append ever drifted from what commit_table wrote, the
  // resident would diverge and this fails. And they grow: the carry is live.
  const finalHex = (final.logsPacked ?? '').replace(/^\\x/, '').toLowerCase();
  for (const hex of fed) {
    assert.ok(finalHex.startsWith(hex),
      `resident belief log drifted from logs_packed: a fed buffer (${hex.length / 2} B) is not a prefix of the final persisted log (${finalHex.length / 2} B)`);
  }
  for (let i = 1; i < fed.length; i++) {
    assert.ok(fed[i].length >= fed[i - 1].length, `the fed log never shrinks within a segment (cycle ${i})`);
  }
});
