// Microbench: per-move SERVER COMPUTE, legacy JS path vs the packed kernel
// pipeline (docs/PACKED_WIRE_CUTOVER.md) vs the raw kernel floor.
//
//   legacy : what one move cost the isolate before the cutover — kernel apply
//            wrapped in marshals + buildEvents JS snapshots + per-viewer TS
//            event encoding + the commit re-marshal (serializeGameState).
//   packed : one runPackedAction call from the previous move's blob — apply,
//            win-finalize, durable blob, masked logwire, per-viewer event
//            buffers — plus the hex conversions the real path pays.
//   floor  : the same call with zero human viewers (spectator buffer only) —
//            approximately the raw C apply+serialize cost.
//
//   BENCH_MOVES=20000 BENCH_PLAYERS=4 node --import tsx e2e/bench_packed.ts
import {
  Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../supabase/functions/_shared/core/types.ts';
import { start_game } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import {
  kernelLegalMoves, kernelShouldAct, serializeGameState, runPackedAction,
  __setKernelSeedSource,
} from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import { handleAttack } from '../supabase/functions/_shared/common/actions/attack.ts';
import { handleCover } from '../supabase/functions/_shared/common/actions/cover.ts';
import { handlePass } from '../supabase/functions/_shared/common/actions/pass.ts';
import { handlePickup } from '../supabase/functions/_shared/common/actions/pickup.ts';
import { handleGood } from '../supabase/functions/_shared/common/actions/good.ts';
import { encodeAction, AwireKindName } from '../supabase/functions/_shared/sdk/ts/wire/awire.ts';
import { encodeEventWire } from '../supabase/functions/_shared/sdk/ts/wire/evwire.ts';
import { logsFromKernelExport } from '../supabase/functions/_shared/sdk/ts/wire/logwire.ts';
import { bytesToBareHex } from '../supabase/functions/_shared/sdk/ts/wire/bytes.ts';
import { bytesToHex } from '../supabase/functions/_shared/common/replay/codec.ts';

const MOVES = Number(process.env.BENCH_MOVES || 20000);
const PLAYERS = Number(process.env.BENCH_PLAYERS || 4);

let seed = 0xfeed;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);
__setKernelSeedSource(() => 4242);

const mkGame = (): Game => {
  const g: Game = {
    id: 'bench', name: 'bench', status: GAME_STATUS.WAITING,
    players: Array.from({ length: PLAYERS }, (_, i): PrivatePlayer => ({
      player_id: `player-${i}`, name: `P${i}`, status: PLAYER_STATUS.READY,
      is_ai: false, hand: [], awaiting_attack: false, hand_length: 0,
      strategy_key: STRATEGY_KEY.HUMAN,
    })),
    deck: [], deck_length: 0, discard_pile_length: 0, flipped: null,
    power_suit: 0, first_attacker: 0, defender: 0, table_battles: [],
    elimination_order: [], good_timestamp: null, good_players: [], logs: [],
  };
  start_game(g); g.status = GAME_STATUS.PLAYING;
  return g;
};

type Mode = 'legacy' | 'packed' | 'floor';
function run(mode: Mode, target: number): { moves: number; ns: bigint } {
  let moves = 0; let ns = 0n;
  const humanSeats = Array.from({ length: PLAYERS }, (_, i) => i);
  const aiMask = 0;
  while (moves < target) {
    let game = mkGame();
    let blob = serializeGameState(game);
    for (let mv = 0; mv < 600 && moves < target; mv++) {
      const actors = game.players.filter(p => kernelShouldAct(game, p.player_id));
      if (actors.length === 0) break;
      const actor = actors[ri(actors.length)];
      const seatIdx = game.players.findIndex(p => p.player_id === actor.player_id);
      const menu = kernelLegalMoves(game, actor.player_id).filter(m => m.type !== 'wait');
      if (menu.length === 0) continue;
      const m = menu[ri(menu.length)];
      const kind = m.type as AwireKindName;

      const t0 = process.hrtime.bigint();
      if (mode === 'legacy') {
        // The old per-move isolate cost: handler (marshal + apply + parse +
        // buildEvents snapshots + appendLogs), per-viewer TS event encoding,
        // commit re-marshal.
        game.logs = [];
        let events;
        try {
          switch (kind) {
            case 'attack': events = handleAttack(game, actor.player_id, m.cards!); break;
            case 'cover': events = handleCover(game, actor.player_id, m.cards!, m.attack_cards!); break;
            case 'pass': events = handlePass(game, actor.player_id, m.cards!); break;
            case 'pickup': events = handlePickup(game, actor.player_id); break;
            default: events = handleGood(game, actor.player_id); break;
          }
        } catch { continue; } // rare menu/handler edge — skip, don't count
        if (events.length > 0) {
          for (const seat of humanSeats) encodeEventWire(events, game, seat, seatIdx);
          encodeEventWire(events, game, -1, seatIdx);
        }
        bytesToHex(serializeGameState(game));
      } else {
        const wire = encodeAction({ kind, cards: m.cards, attack_cards: m.attack_cards });
        const run = runPackedAction(blob, seatIdx, wire, aiMask, mode === 'floor' ? [] : humanSeats);
        if (!run.ok) { continue; } // same rare edge: rejected, skip uncounted
        if (run.ok) {
          bytesToHex(run.stateBlob);
          if (run.logsWire.length > 2) bytesToBareHex(logsFromKernelExport(run.logsWire, 1_700_000_000_000));
          blob = run.stateBlob;
        }
      }
      ns += process.hrtime.bigint() - t0;
      moves++;

      if (mode !== 'legacy') {
        // Advance the JS mirror (outside the timer) so eligibility/menus stay
        // correct for the next iteration.
        game.logs = [];
        try {
          switch (kind) {
            case 'attack': handleAttack(game, actor.player_id, m.cards!); break;
            case 'cover': handleCover(game, actor.player_id, m.cards!, m.attack_cards!); break;
            case 'pass': handlePass(game, actor.player_id, m.cards!); break;
            case 'pickup': handlePickup(game, actor.player_id); break;
            default: handleGood(game, actor.player_id); break;
          }
        } catch { break; }
        blob = serializeGameState(game);
      }
      if (game.status !== GAME_STATUS.PLAYING) break;
    }
  }
  return { moves, ns };
}

// Warm up the kernel + JIT, then measure.
for (const mode of ['legacy', 'packed', 'floor'] as Mode[]) { seed = 0xfeed; run(mode, 1500); }
console.log(`packed-pipeline bench: players=${PLAYERS} moves=${MOVES} (per-move server compute, broadcast/DB excluded)`);
const out: Record<string, number> = {};
for (const mode of ['legacy', 'packed', 'floor'] as Mode[]) {
  seed = 0xfeed;
  const { moves, ns } = run(mode, MOVES);
  const us = Number(ns) / 1000 / moves;
  out[mode] = us;
  console.log(`  ${mode.padEnd(6)} ${us.toFixed(1).padStart(7)} µs/move   ${(1e6 / us | 0).toString().padStart(8)} moves/sec`);
}
console.log(`  speedup legacy→packed: ${(out.legacy / out.packed).toFixed(1)}x   kernel floor: ${(out.legacy / out.floor).toFixed(1)}x`);
