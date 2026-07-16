// M8 overlay interleave gate (docs/BOTS_WASM_MEMORY_PLAN.md).
//
// bots.wasm aliases its replay-call scratch (g_rec / g_bn / g_replay_io,
// 90.5 KiB) INTO the solver arena solve_ws — the two are never live at once
// (wasm_choose_move vs wasm_replay_encode/decode are non-nesting top-level
// exports). This test is the one check that aliasing uniquely needs: it drives
// BOTH families on the SAME adopted bots.wasm instance, interleaved, and proves
// neither corrupts the other. If the overlay offsets ever collide with live
// solver state — or a future edit makes replay read before it writes — the
// encode after a solver burst diverges and this fails.
//
// (The straight wire-format correctness of the overlaid codec is covered by
// replay_codec.test.ts, which runs encode/decode through the same adopted
// bots.wasm; this file adds only the interleave dimension.)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { game_done } from '../supabase/functions/_shared/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, StrategyKey } from '../supabase/functions/_shared/types.ts';
import { shouldBotActCore, processBotAction } from '../supabase/functions/_shared/pure_bot_actions.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';
import { ReplayInput } from '../supabase/functions/_shared/replay/core.ts';
import { encodeReplay } from '../supabase/functions/_shared/replay/encode.ts';
import { decodeReplay } from '../supabase/functions/_shared/replay/decode.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const mkGame = (np: number, strat: StrategyKey): Game => ({
  players: Array.from({ length: np }, (_, i): PrivatePlayer => ({
    player_id: `bot_${i}`, name: `Bot ${i}`, status: PLAYER_STATUS.READY, is_ai: true,
    hand: [], awaiting_attack: false, hand_length: 0, strategy_key: strat,
  })),
  deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
});

async function playToEnd(np: number, strat: StrategyKey): Promise<Game | null> {
  const g = mkGame(np, strat);
  start_game(g);
  let guard = 0;
  while (game_done(g) === null && ++guard < 4000) {
    let acted = false;
    for (let i = 0; i < g.players.length; i++) {
      const p = g.players[i];
      if (shouldBotActCore(g, p, i) && calculateLegalMoves(g, p.player_id).length > 0) {
        if (await processBotAction(g, p)) { acted = true; break; }
      }
    }
    if (!acted) break;
  }
  return game_done(g) !== null ? g : null;
}

// A burst of solver decisions on a fresh game — this is what writes all over
// solve_ws (and therefore all over the aliased replay scratch) between the two
// encodes below. Use the heaviest MC families so the endgame solver actually
// runs and fills the arena — every one of these must be a bot bots.wasm really
// links, or it hammers nothing. ('semtex' used to be here and is not in the
// module: it dispatched to `random`, which never touches the solver at all.)
async function hammerSolver(): Promise<void> {
  for (const strat of ['octogen', 'blackpowder', 'cordite'] as StrategyKey[]) {
    const g = mkGame(2, strat);
    start_game(g);
    let guard = 0;
    while (game_done(g) === null && ++guard < 400) {
      let acted = false;
      for (let i = 0; i < g.players.length; i++) {
        const p = g.players[i];
        if (shouldBotActCore(g, p, i)) { if (await processBotAction(g, p)) { acted = true; break; } }
      }
      if (!acted) break;
    }
  }
}

test('M8: replay encode is byte-identical before and after a solver burst on the same instance', async () => {
  // A finished game gives a non-trivial replay stream to encode. Playing it
  // also adopts bots.wasm as the engine, so the encodes below hit the OVERLAID
  // module (not rules.wasm).
  let game: Game | null = null;
  for (let np = 2; np <= 6 && !game; np++) game = await playToEnd(np, 'octogen' as StrategyKey);
  assert.ok(game, 'could not produce a finished game to encode');

  const input: ReplayInput = {
    playerIds: game.players.map((p) => p.player_id),
    logs: game.logs,
    flipped: game.flipped,
  };

  const e1 = await encodeReplay(input);          // encode #1
  await hammerSolver();                           // scribble all over solve_ws == the replay scratch
  const e2 = await encodeReplay(input);           // encode #2 — same input, post-burst

  assert.equal(e2.x, e1.x, 'encode diverged after a solver burst — overlay corruption');
  assert.equal(e2.base32, e1.base32, 'encode base32 diverged after a solver burst');

  // And the reverse direction: a decode sandwiched by solver work must be stable
  // and reproduce encode #1 (proves decode re-inits its scratch each call too).
  const d1 = await decodeReplay(e1.x);
  await hammerSolver();
  const d2 = await decodeReplay(e1.x);
  assert.equal(d2.logs.length, d1.logs.length, 'decode length diverged after a solver burst');
  assert.deepEqual(
    d2.logs.map((l) => [l.log_type, l.seat, l.card_pairs.length]),
    d1.logs.map((l) => [l.log_type, l.seat, l.card_pairs.length]),
    'decode stream diverged after a solver burst — overlay corruption',
  );

  // Interleave the two families move-by-move: encode, choose, encode, choose …
  // and assert the encode never budges. This is the tightest form of the check.
  for (let k = 0; k < 3; k++) {
    const g = mkGame(2, 'blackpowder' as StrategyKey);
    start_game(g);
    for (let step = 0; step < 12 && game_done(g) === null; step++) {
      for (let i = 0; i < g.players.length; i++) {
        const p = g.players[i];
        if (shouldBotActCore(g, p, i)) { await processBotAction(g, p); break; }
      }
      const en = await encodeReplay(input);
      assert.equal(en.x, e1.x, `encode diverged mid-game at burst ${k} step ${step}`);
    }
  }
});
