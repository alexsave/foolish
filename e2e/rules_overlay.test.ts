// R1 overlay interleave gate (docs/RULES_GUARDS_WASM_MEMORY_PLAN.md §4.4.5).
//
// rules.wasm aliases its replay-call scratch family (g_rec / g_bn /
// g_replay_io) OVER its action family (g_moves / g_snaps / g_io) in one arena
// (src/rules_overlay.h). The two families are never live at the same time —
// wasm_replay_encode/decode vs the action/menu/marshal exports are non-nesting
// top-level calls on a single-threaded instance. This is the one check that
// the aliasing uniquely needs: drive BOTH families on the SAME rules.wasm
// instance, interleaved, and prove neither corrupts the other.
//
// It runs entirely on rules.wasm: this file never loads bots.wasm (no
// processBotAction / bot_strategy import), so engine() stays the rules kernel
// for every kernelLegalMoves / encodeReplay / decodeReplay / serializeGameState
// call below. The straight wire-format correctness of the overlaid codec is
// covered by replay_codec.test.ts; this file adds only the interleave +
// menu/state/snapshot-stability dimensions.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, StrategyKey } from '../server/api/core/types.ts';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import { kernelLegalMoves, serializeGameState } from '../sdk/ts/wasm/engine.ts';
import { ReplayInput } from '../server/api/common/replay/core.ts';
import { encodeReplay } from '../server/api/common/replay/encode.ts';
import { decodeReplay } from '../server/api/common/replay/decode.ts';
import { codeToGame } from '../server/api/common/replay/codec.ts';
import { TUTORIAL_MOVES_CODE } from '../src/components/tutorialGame.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; console.info = () => {}; }

const mkGame = (np: number): Game => ({
  players: Array.from({ length: np }, (_, i): PrivatePlayer => ({
    player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: false,
    hand: [], awaiting_attack: false, hand_length: 0, strategy_key: 'human' as StrategyKey,
  })),
  deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
});

// A real replay stream from the shipped tutorial game, rebuilt as an
// encode-input so BOTH replay directions run against rules.wasm. encodeReplay
// must round-trip the decoded stream back to the same integer (this is the
// verifyRoundTrip contract), which also gives us a strong codec-correctness
// check for free.
async function tutorialReplay(): Promise<{ x: bigint; input: ReplayInput }> {
  const x = codeToGame(TUTORIAL_MOVES_CODE);
  const dec = await decodeReplay(x);            // decode direction — replay family
  const n = dec.playerCount;
  const playerIds = Array.from({ length: n }, (_, i) => `p${i}`);
  const input: ReplayInput = {
    playerIds,
    // deno-lint-ignore no-explicit-any
    logs: dec.logs.map((l: any) => ({
      log_type: l.log_type,
      player_id: l.seat >= 0 && l.seat < n ? playerIds[l.seat] : null,
      card_pairs: l.card_pairs,
      defender_index: l.defender_index ?? null,
    })) as ReplayInput['logs'],
    flipped: dec.trumpCard,
  };
  return { x, input };
}

// One full replay round-trip on the resident (rules) instance — this writes all
// over g_rec / g_bn / g_replay_io, i.e. all over the aliased action-family
// bytes, between the menu/state reads around it.
async function hammerReplay(x: bigint, input: ReplayInput): Promise<void> {
  const enc = await encodeReplay(input);
  assert.equal(enc.x, x, 'encode(decode(tutorial)) diverged — replay codec / overlay corruption');
  const dec = await decodeReplay(x);
  assert.ok(dec.logs.length > 0, 'decode produced an empty stream — overlay corruption');
}

// A dealt game's opening menu is the action family's g_moves; serializeGameState
// exercises g_io; start_game's animation events come from the g_snaps ring via
// g_io. All three must be identical whether or not a replay call ran in between.
test('R1 overlay: menu + state stay byte-identical across an interleaved replay round-trip', async () => {
  const { x, input } = await tutorialReplay();

  for (let np = 2; np <= 6; np++) {
    const g = mkGame(np);
    start_game(g);                                  // deal on rules.wasm (g_snaps + g_io)
    const pid = g.players[g.first_attacker].player_id;

    const menuA = kernelLegalMoves(g, pid);         // g_moves
    const stateA = serializeGameState(g);           // g_io
    assert.ok(menuA.length > 0, `np=${np}: dealt first-attacker has no legal moves`);

    await hammerReplay(x, input);                    // scribble the aliased replay bytes

    const menuB = kernelLegalMoves(g, pid);
    const stateB = serializeGameState(g);
    assert.deepEqual(menuB, menuA, `np=${np}: menu changed across an interleaved replay — overlay corruption`);
    assert.deepEqual([...stateB], [...stateA], `np=${np}: state marshal changed across an interleaved replay`);
  }
});

// The tightest form: interleave the two families call-by-call and assert the
// menu never budges, and — after every replay call — a freshly dealt game still
// produces a well-formed snapshot/animation stream (covers proof obligation P1:
// the snapshot ring survives an aliased replay call).
test('R1 overlay: call-by-call interleave keeps menus stable and snapshots intact', async () => {
  const { x, input } = await tutorialReplay();

  const base = mkGame(4);
  start_game(base);
  const basePid = base.players[base.first_attacker].player_id;
  const baseMenu = kernelLegalMoves(base, basePid);
  assert.ok(baseMenu.length > 0, 'base game has no opening menu');

  for (let k = 0; k < 6; k++) {
    await hammerReplay(x, input);

    // The resident base game's menu must still enumerate identically.
    assert.deepEqual(kernelLegalMoves(base, basePid), baseMenu, `menu diverged at interleave ${k}`);

    // A fresh deal immediately after a replay call must still fill the snapshot
    // ring (start_game emits DEAL/FLIPPED animation events built from g_snaps).
    const g = mkGame(2 + (k % 5));
    const events = start_game(g);
    assert.ok(events.length > 0, `deal after replay ${k} produced no animation events (snapshot ring corrupt)`);
    for (const p of g.players) {
      assert.ok(p.hand.length > 0, `deal after replay ${k}: a player was dealt no cards`);
    }
    assert.ok(g.flipped && g.flipped.suit >= 0, `deal after replay ${k}: no trump flipped`);
    // And that fresh game's menu interleaves cleanly too.
    const gMenu = kernelLegalMoves(g, g.players[g.first_attacker].player_id);
    assert.ok(gMenu.length > 0, `deal after replay ${k}: first attacker has no legal moves`);
  }
});
