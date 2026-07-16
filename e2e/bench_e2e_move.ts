// End-to-end per-move wall clock against REAL Postgres: the full server path
// minus HTTP/auth — row load, kernel, CAS commit, broadcast (shimmed local).
//
//   legacy : executeWithGameLock + the JSON-path handler (what `action` did
//            before the cutover).
//   packed : executePackedAction (what the binary `action` path does now).
//
//   BENCH_E2E_MOVES=300 node --import tsx e2e/bench_e2e_move.ts
import './harness.ts';
import { applySchema, resetDb, seedGame, uuid } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../supabase/functions/_shared/adapter/utils.ts';
import { handleMetaAction } from '../supabase/functions/_shared/adapter/meta_actions.ts';
import { handleAttack } from '../supabase/functions/_shared/common/actions/attack.ts';
import { handleCover } from '../supabase/functions/_shared/common/actions/cover.ts';
import { handlePass } from '../supabase/functions/_shared/common/actions/pass.ts';
import { handlePickup } from '../supabase/functions/_shared/common/actions/pickup.ts';
import { handleGood } from '../supabase/functions/_shared/common/actions/good.ts';
import { executePackedAction } from '../supabase/functions/_shared/adapter/packed_action.ts';
import { encodeAction, AwireKindName } from '../sdk/ts/wire/awire.ts';
import { kernelLegalMoves, kernelShouldAct } from '../sdk/ts/wasm/engine.ts';
import { GAME_STATUS } from '../supabase/functions/_shared/core/types.ts';
import { pgPool } from './harness.ts';

const MOVES = Number(process.env.BENCH_E2E_MOVES || 300);
const say = (line: string) => process.stdout.write(line + '\n'); // harness silences console.log
let seed = 0xabcd;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);

async function freshGame(): Promise<{ gameId: string; pids: string[] }> {
  const gameId = `b${uuid().slice(0, 5)}`;
  const pids = [uuid(), uuid(), uuid(), uuid()];
  await seedGame(gameId, pids.map((id, i) => ({ id, name: `P${i}`, is_ai: false, strategy_key: 'human' })));
  await executeWithGameLock(gameId, async (game) =>
    handleMetaAction({ user: { id: pids[0] } as any, user_name: 'P0', body: { type: 'start', game_id: gameId }, game, reqId: 'b' }), 'b', false);
  return { gameId, pids };
}

type Mode = 'legacy' | 'packed';
async function run(mode: Mode): Promise<{ moves: number; ns: bigint }> {
  let moves = 0; let ns = 0n;
  while (moves < MOVES) {
    const { gameId } = await freshGame();
    for (let mv = 0; mv < 400 && moves < MOVES; mv++) {
      // Pick the move OUTSIDE the timer (a client does this locally).
      const g = await loadCompleteGame(gameId);
      if (g.status !== GAME_STATUS.PLAYING) break;
      const actors = g.players.filter(p => kernelShouldAct(g, p.player_id));
      if (actors.length === 0) break;
      const actor = actors[ri(actors.length)];
      const menu = kernelLegalMoves(g, actor.player_id).filter(m => m.type !== 'wait');
      if (menu.length === 0) continue;
      const m = menu[ri(menu.length)];
      const kind = m.type as AwireKindName;

      const t0 = process.hrtime.bigint();
      try {
        if (mode === 'packed') {
          const wire = encodeAction({ kind, cards: m.cards, attack_cards: m.attack_cards });
          await executePackedAction(gameId, actor.player_id, wire, 'bench');
        } else {
          await executeWithGameLock(gameId, async (game) => {
            let events;
            switch (kind) {
              case 'attack': events = handleAttack(game, actor.player_id, m.cards!); break;
              case 'cover': events = handleCover(game, actor.player_id, m.cards!, m.attack_cards!); break;
              case 'pass': events = handlePass(game, actor.player_id, m.cards!); break;
              case 'pickup': events = handlePickup(game, actor.player_id); break;
              default: events = handleGood(game, actor.player_id); break;
            }
            return { game, events };
          }, 'bench', true);
        }
        ns += process.hrtime.bigint() - t0;
        moves++;
      } catch { /* rare menu/handler edge — skip uncounted */ }
    }
  }
  return { moves, ns };
}

(async () => {
  await applySchema();
  say(`end-to-end move bench vs real Postgres: 4 humans, ${MOVES} moves/mode (load+kernel+CAS commit+shimmed broadcast)`);
  for (const mode of ['legacy', 'packed'] as Mode[]) {
    await resetDb();
    seed = 0xabcd;
    await run(mode); // warmup? full run is cheap enough at 300; do a short warm pass
    await resetDb();
    seed = 0xabcd;
    const { moves, ns } = await run(mode);
    const us = Number(ns) / 1000 / moves;
    say(`  ${mode.padEnd(6)} ${(us / 1000).toFixed(2).padStart(7)} ms/move   (${moves} moves)`);
  }
  await pgPool.end();
})();
