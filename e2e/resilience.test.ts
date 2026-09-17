// Resilience of the server's optimistic-concurrency core (table_io.ts runTableOp
// + the version-fenced commit_table RPC) against the failure modes it is built to
// survive: a concurrent writer committing under us, sustained write contention,
// a vanished game, a move landing on an already-finished game, and the
// best-effort end-of-game side effects (ELO + replay snapshot) actually landing.
//
// Nothing here mocks the database: it is the real commit_table plpgsql and the
// real loader running in Postgres, on kernel-owned rows. Contention is injected
// at the one transport seam the server has, supabaseClient.rpc: a second writer
// bumps games.version immediately before the server's own commit_table call
// reaches Postgres, which is exactly "another request committed between our
// load and our commit", deterministically. (The operation itself is a
// synchronous kernel section now, so it can no longer await a writer inside
// itself the way the old executeWithGameLock callback did.)

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, uuid, pgPool, broadcastLog } from './harness.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import {
  commitProducts, loadRow, runTableOp, __setTableDealSeedOverride,
} from '../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import { supabaseClient } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { __clearGameCache } from '../server/impls/supabase/functions/_shared/adapter/game_cache.ts';
import { fixture, fixtureTable, GAME_OVER, IDLE } from './helpers/table_fixture.ts';
import { seedTable } from './helpers/table_db.ts';
import { legalMoves, mustReadTable } from './helpers/table_play.ts';
import { runAction, runMeta, seedLobby } from './helpers/table_server.ts';
import { __botCycle } from '../server/impls/supabase/functions/_shared/adapter/bot_actions.ts';
import { serverTable } from '../sdk/ts/table/server_table.ts';
import { parseBeliefProbe } from '../sdk/ts/wasm/bots.ts';
import { ACTION_STATUS, AWIRE_KIND } from '../sdk/ts/wire/awire.ts';
import { suiteRng } from './helpers/rng.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const rng = suiteRng('resilience');
const pick = rng.pick;
const dbVersion = async (id: string): Promise<number> =>
  Number((await pgPool.query('SELECT version FROM games WHERE id=$1', [id])).rows[0].version);
const bumpVersion = (id: string) => pgPool.query('UPDATE games SET version = version + 1 WHERE id=$1', [id]);

// Every deal draws its seed from the suite stream, so a red run replays its deals.
const dealSeed = () => Uint8Array.from({ length: 32 }, () => rng.int(256));

/**
 * Runs `body` with a second writer injected before the server's commit_table
 * calls: `stomp(n)` is asked, per call (1-based), whether to bump the version
 * first. Returns how many commit_table calls the server made.
 */
async function withStomp(gameId: string, stomp: (call: number) => boolean, body: () => Promise<void>): Promise<number> {
  const client = supabaseClient as unknown as { rpc: (name: string, params?: Record<string, unknown>) => Promise<unknown> };
  const real = client.rpc;
  let calls = 0;
  client.rpc = async (name, params) => {
    if (name === 'commit_table') {
      calls++;
      if (stomp(calls)) await bumpVersion(gameId);
    }
    return real.call(client, name, params);
  };
  try { await body(); } finally { client.rpc = real; }
  return calls;
}

// A dealt game between a human (seat 0) and a random bot, dealt by the kernel
// through the real lobby op and commit. Every seat moves through the move path.
async function startedGame(): Promise<{ id: string; human: string; bot: string }> {
  const id = `rs${uuid().slice(0, 6)}`;
  const human = uuid(), bot = uuid();
  await seedLobby(id, [{ id: human, name: 'H0', ready: false }, { id: bot, name: 'B0', brain: 'random' }]);
  __setTableDealSeedOverride(dealSeed());
  await runMeta(id, human, { type: 'start' });
  assert.equal((await mustReadTable(id)).status, L.GAME_STATUS_PLAYING, 'fixture: the game dealt');
  return { id, human, bot };
}

// A no-op table operation by a seated player: load, commit what was loaded.
// It commits (nothing refuses it), which is all a CAS test needs.
const rereadOp = (gameId: string, counter: { n: number }) => ({
  gameId, reqId: 'resilience', viewerId: null,
  run: () => { counter.n++; return L.TABLE_OK; },
});

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); __clearGameCache(); });
after(async () => { __setTableDealSeedOverride(null); await pgPool.end(); });

test('commit_table fences a stale version and accepts the fresh one', async () => {
  const { id } = await startedGame();
  const row = await loadRow(id, false);
  const staleVersion = row.version;

  // The products of the loaded row, as one kernel section copies them out.
  const table = fixtureTable();
  assert.equal(table.load(row.state, row.roster), L.TABLE_OK);
  const seats = table.seats();
  const products = table.commit(id, staleVersion + 1, 0);
  assert.ok(typeof products !== 'number', 'the loaded table has commit products');

  // A concurrent writer commits first (version moves on).
  await bumpVersion(id);

  assert.equal(await commitProducts(id, { ...row, version: staleVersion }, products, seats, null), null,
    'a stale-version commit is rejected');

  // Reload the now-current version and commit cleanly.
  const fresh = await loadRow(id, false);
  assert.equal(fresh.version, staleVersion + 1);
  const version = await commitProducts(id, fresh, products, seats, null);
  assert.equal(version, fresh.version + 1, 'a fresh-version commit succeeds and the fence bumps the version by one');
  assert.equal(await dbVersion(id), fresh.version + 1);
});

test('runTableOp recovers from a concurrent write (conflict -> reload -> retry -> commit)', async () => {
  const { id } = await startedGame();
  const startVersion = await dbVersion(id);

  const ran = { n: 0 };
  // On the first commit only, another actor commits between our load and our commit.
  const commits = await withStomp(id, (call) => call === 1, async () => {
    const out = await runTableOp(rereadOp(id, ran));
    assert.equal(out.committed, true, 'the redone operation committed');
    assert.equal(out.version, startVersion + 2, 'the outcome carries the committed version');
  });

  assert.equal(ran.n, 2, `the operation was run again after the conflict (ran ${ran.n}x)`);
  assert.equal(commits, 2, 'one conflicting commit, one that landed');
  // startVersion +1 (the injected writer) +1 (our eventual successful commit).
  assert.equal(await dbVersion(id), startVersion + 2, 'exactly the injected write + our redone commit landed');
});

test('a real move recovers from a concurrent write the same way', async () => {
  const { id, human } = await startedGame();
  const t0 = await mustReadTable(id);
  const mv = legalMoves(t0, (s) => s.id === human)[0] ?? legalMoves(t0)[0];
  assert.ok(mv, 'fixture: somebody can move');
  let res!: Awaited<ReturnType<typeof runAction>>;
  const commits = await withStomp(id, (call) => call === 1, async () => { res = await runAction(id, mv.playerId, mv); });
  assert.equal(res.status, ACTION_STATUS.APPLIED, 'the move applied after the retry');
  assert.equal(commits, 2);
  assert.equal(res.version, t0.version + 2, 'the response carries the version the move committed at');
  assert.equal(await dbVersion(id), t0.version + 2);
});

test('a bot cycle that loses its commit replays the moves it already chose instead of searching again', async () => {
  // A human and a cordite bot on a pinned deal.
  const id = `rb${uuid().slice(0, 6)}`;
  const human = uuid(), bot = uuid();
  await seedLobby(id, [{ id: human, name: 'H0', ready: false }, { id: bot, name: 'B0', brain: 'cordite' }]);
  __setTableDealSeedOverride(Uint8Array.from({ length: 32 }, (_, i) => (i * 53 + 11) & 0xff));
  await runMeta(id, human, { type: 'start' });

  const cols = 'state, roster, status, needs_bots, version, round_epoch, game_seed, logs_packed';
  const snapshot = async () => (await pgPool.query(`SELECT ${cols} FROM games WHERE id = $1`, [id])).rows[0];
  const restore = async (r: Record<string, unknown>) => {
    await pgPool.query(
      `UPDATE games SET state = $2, roster = $3, status = $4, needs_bots = $5, version = $6, round_epoch = $7, game_seed = $8, logs_packed = $9 WHERE id = $1`,
      [id, r.state, r.roster, r.status, r.needs_bots, r.version, r.round_epoch, r.game_seed, r.logs_packed]);
    __clearGameCache();
  };

  // Every decision the drive SEARCHES for is recorded by the kernel's probe on the
  // server's table instance; a move replayed from the lost attempt searches nothing.
  const table = await serverTable();
  const cycle = async (stompFirst: boolean) => {
    table.__beliefProbeReset();
    const commits = await withStomp(id, (call) => stompFirst && call === 1, () => __botCycle(id));
    const dump = table.__beliefProbeDump();
    const searches = parseBeliefProbe(dump.bytes, dump.n).map((r) => `${r.seat}:${r.nLogs}:${[...r.cards].sort().join(',')}`);
    const after = (await pgPool.query('SELECT state, logs_packed, version FROM games WHERE id = $1', [id])).rows[0];
    return { commits, searches, after };
  };

  // The first cycle in which the bot has a move to make, run cleanly: the human
  // moves until then.
  let before = await snapshot();
  let clean = await cycle(false);
  for (let step = 0; clean.commits === 0 && step < 40; step++) {
    const mv = legalMoves(await mustReadTable(id), (s) => s.id === human)[0];
    assert.ok(mv, `fixture: the human can move while the bot has none (step ${step})`);
    assert.equal((await runAction(id, human, mv)).status, ACTION_STATUS.APPLIED);
    before = await snapshot();
    clean = await cycle(false);
  }
  assert.equal(clean.commits, 1, 'fixture: a bot cycle committed');
  assert.ok(clean.searches.length > 0, 'the bot searched for its move');

  // The same cycle again from the same row, now losing its first commit.
  await restore(before);
  const retried = await cycle(true);
  assert.equal(retried.commits, 2, 'the first commit lost to a concurrent writer, the retry landed');
  assert.deepEqual(retried.searches, clean.searches,
    'the retry searched nothing: every decision of the cycle was searched once, by the attempt that lost');
  assert.equal(retried.after.state, clean.after.state, 'and the retry committed the same moves');
  assert.equal(retried.after.logs_packed.length, clean.after.logs_packed.length, 'the same number of log records');
  assert.equal(Number(retried.after.version), Number(clean.after.version) + 1, 'one extra version: the injected writer');
});

test('runTableOp gives up cleanly under sustained contention (bounded, no hang)', async () => {
  const { id } = await startedGame();
  const ran = { n: 0 };
  await assert.rejects(
    withStomp(id, () => true, async () => { await runTableOp(rereadOp(id, ran)); }),
    /write contention/i,
    'exhausting the retries surfaces a clean error, not a hang',
  );
  assert.equal(ran.n, 5, 'bounded at exactly MAX_ATTEMPTS (5), never an unbounded spin');
});

test('a missing game is a clean "not found", on the loader and on the move path', async () => {
  const ghost = `ghost-${uuid().slice(0, 6)}`;
  await assert.rejects(loadRow(ghost, false), /not found/i);
  await assert.rejects(runAction(ghost, uuid(), Uint8Array.of(AWIRE_KIND.pickup, 0)), /not found/i);
});

test('a move on an already-finished game is a moot no-op, never a crash', async () => {
  const id = `ro${uuid().slice(0, 6)}`;
  const a = uuid(), b = uuid();
  await seedTable(id, fixture().seats([{ id: a, name: 'A' }, { id: b, name: 'B' }])
    .status(GAME_OVER).eliminated(0).discard(36).seatStatus(0, IDLE).seatStatus(1, IDLE).build(), { version: 7 });
  const before = await mustReadTable(id);
  const sent = broadcastLog.length;

  for (const wire of [Uint8Array.of(AWIRE_KIND.pickup, 0), Uint8Array.of(AWIRE_KIND.good, 0), Uint8Array.of(AWIRE_KIND.attack, 1, 4)]) {
    const res = await runAction(id, a, wire);
    assert.equal(res.status, ACTION_STATUS.MOOT, 'the kernel answers moot');
    assert.equal(res.version, 7, 'the response carries the stored version');
    assert.equal(res.needsBots, false);
  }
  const after = await mustReadTable(id);
  assert.equal(after.version, before.version, 'nothing committed');
  assert.deepEqual(after.state, before.state, 'the finished state is left as-is');
  assert.equal(broadcastLog.length, sent, 'no events broadcast');
});

test('a full game commits GAME_OVER and lands its end-of-game side effects (ELO + snapshot)', async () => {
  // Bots only, as before: the deal is a lobby ready by a seated bot's id, which
  // no client can send (a bot has no auth account) but is the kernel's own deal
  // through the real meta handler and commit_table.
  const id = `rf${uuid().slice(0, 6)}`;
  const b0 = uuid(), b1 = uuid();
  await seedLobby(id, [{ id: b0, name: 'B0', brain: 'random' }, { id: b1, name: 'B1', brain: 'random' }]);
  __setTableDealSeedOverride(dealSeed());
  await runMeta(id, b0, { type: 'start' });
  assert.equal((await mustReadTable(id)).status, L.GAME_STATUS_PLAYING, 'fixture: the bots-only game dealt');

  let last: ReturnType<typeof legalMoves>[number] | null = null;
  for (let step = 0; step < 300; step++) {
    const t = await mustReadTable(id);
    if (t.status !== L.GAME_STATUS_PLAYING) break;
    const moves = legalMoves(t);
    if (moves.length === 0) break;
    const mv = pick(moves);
    last = mv;
    await runAction(id, mv.playerId, mv);
  }

  const final = await mustReadTable(id);
  assert.equal(final.statusColumn, 'game_over',
    `the game reached and durably committed GAME_OVER (seed=${rng.seed})`);
  assert.equal(final.needsBotsColumn, false, 'a finished game has no bot work');

  // A move after the end is moot.
  assert.ok(last, 'moves were made');
  assert.equal((await runAction(id, last!.playerId, last!)).status, ACTION_STATUS.MOOT, 'a late move on the finished game is moot');

  // The replay snapshot is a best-effort side effect, but on the happy path it
  // must land (proving finalizeEndedGame ran to completion) and retire the log.
  const snaps = Number((await pgPool.query('SELECT count(*)::int AS n FROM game_snapshots WHERE game_id=$1', [id])).rows[0].n);
  assert.equal(snaps, 1, `exactly one replay snapshot row was written (seed=${rng.seed})`);
  assert.equal((await mustReadTable(id)).logsPacked, '', `the session log was retired after the snapshot (seed=${rng.seed})`);

  // The rating update ran: the two bots no longer sit at the 1000 default.
  const elos = (await pgPool.query('SELECT elo_rating, games_played FROM bots')).rows;
  assert.equal(elos.length, 2);
  assert.ok(elos.some((r: { elo_rating: number }) => r.elo_rating !== 1000),
    `at least one bot rating moved off the default (seed=${rng.seed})`);
  assert.ok(elos.every((r: { games_played: number }) => r.games_played === 1),
    `every bot recorded exactly one played game (seed=${rng.seed})`);
});

test('a rating never goes below zero: a loser already at 0 stays at 0, the winner still gains', async () => {
  const id = `rz${uuid().slice(0, 6)}`;
  const b0 = uuid(), b1 = uuid();
  await seedLobby(id, [{ id: b0, name: 'B0', brain: 'random' }, { id: b1, name: 'B1', brain: 'random' }]);
  await pgPool.query('UPDATE bots SET elo_rating = 0, previous_elo = 0 WHERE id = ANY($1::uuid[])', [[b0, b1]]);
  __setTableDealSeedOverride(Uint8Array.from({ length: 32 }, (_, i) => (i * 71 + 5) & 0xff));
  await runMeta(id, b0, { type: 'start' });
  for (let step = 0; step < 400; step++) {
    const t = await mustReadTable(id);
    if (t.status !== L.GAME_STATUS_PLAYING) break;
    const mv = legalMoves(t)[0];
    assert.ok(mv, `fixture: somebody can move (step ${step})`);
    await runAction(id, mv.playerId, mv);
  }
  assert.equal((await mustReadTable(id)).statusColumn, 'game_over', 'fixture: the game ended');

  const rows = (await pgPool.query('SELECT elo_rating, previous_elo, games_played FROM bots ORDER BY elo_rating')).rows;
  assert.deepEqual(rows.map((r) => [r.previous_elo, r.games_played]), [[0, 1], [0, 1]], 'both bots were rated from 0 for this game');
  assert.equal(rows[0].elo_rating, 0, 'the loser, at 0 already, is held at 0 instead of going negative');
  assert.ok(rows[1].elo_rating > 0, `the winner still gains (${rows[1].elo_rating})`);
});
