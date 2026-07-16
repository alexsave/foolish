// Wiring-level integration test for the belief-bot session log — the layer the
// unit test (belief_logs.test.ts) can't reach.
//
// belief_logs.test.ts pins the KERNEL CONTRACT (importLogs honors belief_logs).
// But the bug that actually shipped was in the WIRING: the production bot loop
// loads state with loadCompleteGame (which leaves game.logs EMPTY) and, for a
// long window, never repopulated it — so the belief bots chose blind. No test
// caught it because the offline arena/eval runs in-memory self-play where
// game.logs accumulates naturally; nothing exercised the real DB-reload path.
//
// This test closes that gap. It drives the REAL server bot loop (lockedBotLoop
// → processBotActions → loadCompleteGame → the belief hydration → the kernel
// chooser) against a REAL Postgres via the harness, and asserts that octogen
// actually SEES the accumulating session log at choose time.
//
// It observes the two halves at the two places they are true:
//
//   - WHAT THE BOT SAW comes from the kernel (wasmBeliefProbe*). The choose step
//     moved in-kernel (bot_drive, F2/A2), so the TS seam this used to patch
//     (WasmBotStrategy.chooseMoveDirect) is no longer on the loop's path — and
//     even when it was, it could only prove the bytes were HANDED OVER, never
//     that the importer spliced them into the Game octogen read. That gap is
//     where "octogen chose blind" lived, so the probe reports the log as the
//     strategy was about to read it.
//   - THE FED BYTES stay observed here, because the resident log's arithmetic
//     (concat-and-carry across cycles) is this side's job, not the kernel's.

import './harness.ts'; // sets Deno globals BEFORE any server module loads
import { test, before, beforeEach, after, mock } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import { __setBotSeedSource } from '../sdk/ts/wasm/bots.ts';
import { __setKernelSeedSource } from '../sdk/ts/wasm/engine.ts';
import { AnimationEvent, Game } from '../server/api/core/types.ts';
import { bytesToBareHex } from '../sdk/ts/wire/bytes.ts';

// The bytes the loop hands the kernel, captured per drive.
// BARE hex (no \x) — matches how logs_packed is stored, so a prefix compare works.
const fed: string[] = [];

// Wrap the ONE call the loop makes into the kernel, so the bytes it hands over
// are captured without touching production code, and hand back a lockedBotLoop
// bound to the wrapper. The mock must be installed before bot_actions.ts is
// imported, since that binds wasmBotDrive at load — hence the dynamic imports
// (tsx transforms these files to CJS, where top-level await is unavailable).
async function wireLoop() {
  const realBots = await import('../sdk/ts/wasm/bots.ts');
  mock.module('../sdk/ts/wasm/bots.ts', {
    namedExports: {
      ...realBots,
      wasmBotDrive: (game: Game, opts: Parameters<typeof realBots.wasmBotDrive>[1]) => {
        const b = game.belief_log_bytes;
        if (opts.logs) fed.push(b ? bytesToBareHex(b).toLowerCase() : '');
        return realBots.wasmBotDrive(game, opts);
      },
    },
  });
  const { lockedBotLoop } = await import('../server/impls/supabase/functions/_shared/adapter/bot_actions.ts');
  return { lockedBotLoop, ...realBots };
}

const mkLcgU32 = (seed: number) => {
  let s = (seed >>> 0) || 1;
  return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s; };
};

before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });
after(() => { __setKernelSeedSource(null); __setBotSeedSource(null); });

// Number of session-log records currently persisted for this game (the source
// of truth a belief bot is supposed to see). Bare-hex packed logs: we only need
// a nonzero/size signal, so decode via the real decoder.
async function persistedLogCount(gameId: string): Promise<number> {
  const row = (await pgPool.query('SELECT logs_packed FROM games WHERE id=$1', [gameId])).rows[0];
  if (!row?.logs_packed) return 0;
  const { decodeLogs } = await import('../sdk/ts/wire/logwire.ts');
  const { hexToBytes } = await import('../server/api/common/replay/codec.ts');
  const players = (await pgPool.query('SELECT player_id FROM player_hands WHERE game_id=$1', [gameId])).rows
    .map((r: { player_id: string }) => ({ player_id: r.player_id }));
  try { return decodeLogs(hexToBytes(row.logs_packed), gameId, players).length; }
  catch { return -1; }
}

test('the server bot loop feeds octogen the whole session log (not an empty one)', async () => {
  // Deterministic deal/draws so the run is reproducible.
  __setKernelSeedSource(mkLcgU32(0x0C704E7));
  __setBotSeedSource(mkLcgU32(0xBEEF11));

  const gameId = `bw${uuid().slice(0, 6)}`;
  // Bots-only so lockedBotLoop drives the whole thing. Both octogen: the bug is
  // about octogen's own memory, and self-play keeps the belief demand high.
  const players = [
    { id: uuid(), name: 'Octo0', is_ai: true, strategy_key: 'octogen' },
    { id: uuid(), name: 'Octo1', is_ai: true, strategy_key: 'octogen' },
  ];
  await seedGame(gameId, players);
  await executeWithGameLock(gameId,
    async (g: Game) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);

  // One drive segment. Octogen is heavy, so the CPU predictor may bail after a
  // handful of cycles — that's fine: a handful is enough for the session log to
  // grow past empty and prove the wiring delivers it.
  const { lockedBotLoop, wasmBeliefProbeReset, wasmBeliefProbeDump } = await wireLoop();
  fed.length = 0;
  wasmBeliefProbeReset();
  await lockedBotLoop(gameId);
  // Every search the kernel ran, with the log as the strategy was about to read it.
  const searches = wasmBeliefProbeDump();

  const sessionLen = await persistedLogCount(gameId);
  const maxSeen = Math.max(0, ...searches.map(s => s.nLogs));
  console.error(`[wiring] octogen searches=${searches.length} maxKernelLogs=${maxSeen} fedBuffers=${fed.length} persistedSessionLen=${sessionLen}`);

  // octogen actually got to choose through the real loop.
  assert.ok(searches.length > 0, 'octogen never chose through the real bot loop');
  // The loop hydrated on every cycle a belief bot was eligible.
  assert.ok(fed.length > 0 && fed.every(h => h.length > 0),
    'belief_log_bytes was empty at a belief-bot drive — hydration wiring missing');

  // THE REGRESSION GUARD: once the session has accumulated records, octogen must
  // see them. Pre-fix, the log the chooser saw was pinned at 0 no matter how long
  // the game ran. The game produces several log records per bout (attack/cover/
  // draw/discard), so a non-trivial max proves the whole session reaches the
  // kernel — and this now asserts it of the Game the strategy read, not of the
  // bytes handed to the importer.
  assert.ok(sessionLen >= 4, `precondition: the drive should persist a real session (got ${sessionLen})`);
  assert.ok(maxSeen >= 4,
    `octogen searched with a near-empty log (max=${maxSeen}) while ${sessionLen} records were persisted — belief bot is running blind`);

  // RESIDENT-LOG CORRECTNESS: the loop carried the log across cycles and appended
  // each committed move's bytes instead of re-reading the DB. Every buffer it fed
  // the kernel must therefore be a byte-exact PREFIX of the final persisted
  // logs_packed — if the append ever drifted from what commit_game wrote, the
  // resident would diverge and this fails.
  const finalHex = ((await pgPool.query('SELECT logs_packed FROM games WHERE id=$1', [gameId])).rows[0]?.logs_packed ?? '').toLowerCase();
  for (const hex of fed) {
    assert.ok(finalHex.startsWith(hex),
      `resident belief log drifted from logs_packed: a fed buffer (${hex.length / 2} B) is not a prefix of the final persisted log (${finalHex.length / 2} B)`);
  }
});
