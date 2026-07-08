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
// actually SEES the accumulating session log at choose time. It spies at the
// exact seam the bug lived behind: what game.belief_logs holds when the bot's
// chooser is invoked. Pre-fix, that was always empty; post-fix it is the
// current session, loaded from games.logs_packed.

import './harness.ts'; // sets Deno globals BEFORE any server module loads
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';

import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock } from '../supabase/functions/_shared/utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { lockedBotLoop } from '../supabase/functions/_shared/bot_actions.ts';
import { WasmBotStrategy } from '../supabase/functions/_shared/bot_strategy.ts';
import { __setBotSeedSource } from '../supabase/functions/_shared/wasm/bots.ts';
import { __setKernelSeedSource } from '../supabase/functions/_shared/wasm/engine.ts';
import { AnimationEvent, Game } from '../supabase/functions/_shared/types.ts';

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
  const { decodeLogs } = await import('../supabase/functions/_shared/wire/logwire.ts');
  const { hexToBytes } = await import('../supabase/functions/_shared/replay/codec.ts');
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

  // Spy at the exact seam: what does game.belief_log_bytes hold when the bot
  // loop asks octogen to choose? Decode the packed bytes to a record count.
  const { decodeLogs } = await import('../supabase/functions/_shared/wire/logwire.ts');
  const seen: { beliefLen: number }[] = [];
  const orig = WasmBotStrategy.prototype.chooseMoveDirect;
  WasmBotStrategy.prototype.chooseMoveDirect = function (game: Game, botPlayerId: string) {
    // `this.logs` is true only for the belief bots (octogen here).
    if ((this as unknown as { logs: boolean }).logs) {
      const bytes = game.belief_log_bytes;
      let cnt = -1;
      if (bytes) { try { cnt = decodeLogs(bytes, game.id, game.players).length; } catch { cnt = -1; } }
      seen.push({ beliefLen: cnt });
    }
    return orig.call(this, game, botPlayerId);
  };

  try {
    // One drive segment. Octogen is heavy, so the CPU predictor may bail after a
    // handful of cycles — that's fine: a handful is enough for the session log to
    // grow past empty and prove the wiring delivers it.
    await lockedBotLoop(gameId);
  } finally {
    WasmBotStrategy.prototype.chooseMoveDirect = orig;
  }

  const sessionLen = await persistedLogCount(gameId);
  console.error(`[wiring] octogen choices=${seen.length} maxBeliefLen=${Math.max(-1, ...seen.map(s => s.beliefLen))} persistedSessionLen=${sessionLen}`);

  // octogen actually got to choose through the real loop.
  assert.ok(seen.length > 0, 'octogen never chose through the real bot loop');
  // Never chose with a broken/undefined belief field.
  assert.ok(seen.every(s => s.beliefLen >= 0), 'belief_log_bytes was undefined at a belief-bot choose — hydration wiring missing');
  // THE REGRESSION GUARD: once the session has accumulated records, octogen must
  // see them. Pre-fix, maxBeliefLen was pinned at 0 no matter how long the game
  // ran. The game produces several log records per bout (attack/cover/draw/
  // discard), so a non-trivial max proves the whole session reaches the chooser.
  const maxBeliefLen = Math.max(0, ...seen.map(s => s.beliefLen));
  assert.ok(sessionLen >= 4, `precondition: the drive should persist a real session (got ${sessionLen})`);
  assert.ok(maxBeliefLen >= 4,
    `octogen chose with a near-empty log (max=${maxBeliefLen}) while ${sessionLen} records were persisted — belief bot is running blind`);
});
