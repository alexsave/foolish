// R1 overlay interleave gate (docs/RULES_GUARDS_WASM_MEMORY_PLAN.md §4.4.5).
//
// rules.wasm aliases its replay-call scratch family (g_rec / g_bn /
// g_replay_io) OVER its action family (g_moves / g_snaps / g_io) in one arena
// (src/rules_overlay.h). The two families are never live at the same time —
// the replay encode/decode exports vs the action/menu/marshal exports are
// non-nesting top-level calls on a single-threaded instance. This is the one
// check that the aliasing uniquely needs: drive BOTH families on the SAME
// rules.wasm instance, interleaved, and prove neither corrupts the other.
//
// IT ONLY MEANS ANYTHING ON rules.wasm, and that is not a comment, it is
// asserted below. bots.wasm embeds the same kernel with a DIFFERENT overlay
// (into solve_ws, M8 - covered by replay_solver_overlay.test.ts), and the first
// call into sdk/ts/wasm/bots.ts instantiates it and ADOPTS THE ENGINE SLOT
// (__adoptEngine), silently moving every later engine() call onto it. This file
// once imported kernelB32Decode from there for one line of fixture setup and
// spent that whole time testing the other module's overlay twice. So: nothing
// here touches bots.ts, the base32 the fixture needs is decoded locally, and
// assertEngineIsRules() pins the fact.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bytesToBigint } from '../server/api/common/replay/codec.ts';

import { Game, GAME_STATUS, LOG_TYPE, PLAYER_STATUS, PrivatePlayer, StrategyKey } from '../server/api/core/types.ts';
import { start_game_packed } from '../server/api/common/game_lifecycle.ts';
import {
  __LOG_TYPE_TO_INT, __kernelWasmBytes, kernelLegalMoves, kernelReplayEncodeV6, serializeGameState,
} from '../sdk/ts/wasm/engine.ts';
import { SeatLog, cardId } from '../server/api/common/replay/core.ts';
import { decodeReplay } from '../server/api/common/replay/decode.ts';
import { TUTORIAL_MOVES_CODE } from '../src/components/tutorialGame.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; console.info = () => {}; }

// rules.wasm pins linear memory at 3 pages (196,608 B); bots.wasm runs to ~37.
// Anything above a mebibyte is the big module wearing the engine slot, and this
// file's whole premise with it.
function assertEngineIsRules(where: string): void {
  const bytes = __kernelWasmBytes();
  assert.ok(bytes > 0, `${where}: no kernel loaded`);
  assert.ok(bytes < (1 << 20),
    `${where}: the engine slot holds a ${bytes}-byte module - bots.wasm has adopted it, `
    + 'so this file is testing the M8 overlay, not R1');
}

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

// RFC 4648 base32, the codec's alphabet. The kernel owns the real one
// (replay_b32_decode) but it is only reachable through bots.wasm, and pulling
// the big module in to read one frozen fixture is what broke this file before.
// Ten lines of decoder here cost nothing and the kernel's own base32 is
// asserted where it belongs (replay_codec.test.ts).
const B32_ALPHA = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
function base32Decode(code: string): Uint8Array {
  let bits = 0, value = 0;
  const out: number[] = [];
  for (const ch of code.toUpperCase()) {
    if (ch === '-') break;                       // the extras suffix begins here
    const i = B32_ALPHA.indexOf(ch);
    if (i < 0) continue;
    value = (value << 5) | i;
    bits += 5;
    if (bits >= 8) { out.push((value >> (bits - 8)) & 0xff); bits -= 8; }
  }
  return Uint8Array.from(out);
}

const INFO = [LOG_TYPE.ATTACK, LOG_TYPE.COVER, LOG_TYPE.PASS, LOG_TYPE.PICKUP];

// The encode input (c/src/replay.h) rebuilt from a decoded stream: header, the
// reveal stream, then the actions.
//
// It is buildable at all only because the format is hidden-state-lossless - the
// decoded stream carries every real card, so the deal and the draws can be read
// straight back out of it. Under the retrodiction format this was impossible,
// which is why this file used to encode from the log stream alone and got a
// DIFFERENT (shorter, deal-less) code back; the round-trip target then had to
// be that code's own fixpoint rather than the fixture. Here it is the fixture.
function marshalFromDecoded(logs: SeatLog[], n: number, trumpId: number, firstAttacker: number): Uint8Array {
  const reveals: number[] = [];
  const actions: number[] = [];
  let nActions = 0;
  for (let i = 0; i < logs.length; i++) {
    const l = logs[i];
    if (l.log_type === LOG_TYPE.DRAW) {
      // Every drawn card in draw order. The flip is never a reveal: it IS the
      // header trump, and the coder takes it from there.
      for (const p of l.card_pairs) {
        const id = cardId(p.primary);
        if (id !== trumpId) reveals.push(id);
      }
    }
    if (INFO.includes(l.log_type)) {
      actions.push(__LOG_TYPE_TO_INT.get(l.log_type)!, l.seat ?? 0xff, l.card_pairs.length);
      for (const p of l.card_pairs) actions.push(cardId(p.primary), p.target ? cardId(p.target) : 0xff);
      nActions++;
    } else if (l.log_type === LOG_TYPE.DISCARD && i > 0 && logs[i - 1].log_type === LOG_TYPE.GOOD) {
      actions.push(0xff, 0xff, 0);   // REPLAY_ROUND_END
      nActions++;
    } else if (l.log_type === LOG_TYPE.GOOD) {
      // A good is an atom only while it is still PENDING at the end of the
      // stream; any later action clears it and the decoder rebuilds it for free
      // (c/src/replay.c log_atom_kind).
      let pending = true;
      for (let j = i + 1; j < logs.length; j++) {
        if (logs[j].log_type !== LOG_TYPE.GOOD) { pending = false; break; }
      }
      if (pending) { actions.push(__LOG_TYPE_TO_INT.get(LOG_TYPE.GOOD)!, l.seat ?? 0xff, 0); nActions++; }
    }
  }
  const buf = new Uint8Array(7 + reveals.length + actions.length);
  buf[0] = n;
  buf[1] = trumpId;
  buf[2] = firstAttacker;
  buf[3] = nActions & 0xff;
  buf[4] = (nActions >> 8) & 0xff;
  buf[5] = reveals.length & 0xff;
  buf[6] = (reveals.length >> 8) & 0xff;
  buf.set(reveals, 7);
  buf.set(actions, 7 + reveals.length);
  return buf;
}

// A real replay stream from the shipped tutorial game, rebuilt as an
// encode-input so BOTH replay directions run against rules.wasm.
//
// The round-trip target IS the tutorial constant: there is one C coder, and a
// code re-encoded from its own decode must come back byte-identical. The deck
// tail the fixture carries beyond what the game drew changes nothing - the
// coder pops reveals as draws happen and never reads past them.
async function tutorialReplay(): Promise<{ code: Uint8Array; input: Uint8Array }> {
  const code = base32Decode(TUTORIAL_MOVES_CODE);
  const dec = await decodeReplay(bytesToBigint(code));     // real stream - replay family
  assertEngineIsRules('after the fixture decode');
  const input = marshalFromDecoded(dec.logs, dec.playerCount, cardId(dec.trumpCard), dec.firstAttacker);
  assert.deepEqual([...kernelReplayEncodeV6(input)], [...code],
    're-encoding the tutorial\'s own decoded stream did not reproduce it');
  return { code, input };
}

// One full replay round-trip on the resident (rules) instance — this writes all
// over g_rec / g_bn / g_replay_io, i.e. all over the aliased action-family
// bytes, between the menu/state reads around it.
async function hammerReplay(code: Uint8Array, input: Uint8Array): Promise<void> {
  assert.deepEqual([...kernelReplayEncodeV6(input)], [...code],
    're-encode diverged across the interleave - overlay corruption');
  const dec = await decodeReplay(bytesToBigint(code));
  assert.ok(dec.logs.length > 0, 'decode produced an empty stream — overlay corruption');
}

// A dealt game's opening menu is the action family's g_moves; serializeGameState
// exercises g_io; start_game's animation events come from the g_snaps ring via
// g_io. All three must be identical whether or not a replay call ran in between.
test('R1 overlay: menu + state stay byte-identical across an interleaved replay round-trip', async () => {
  const { code, input } = await tutorialReplay();

  for (let np = 2; np <= 6; np++) {
    const g = mkGame(np);
    start_game_packed(g);                           // deal on rules.wasm (g_snaps + g_io)
    const pid = g.players[g.first_attacker].player_id;

    const menuA = kernelLegalMoves(g, pid);         // g_moves
    const stateA = serializeGameState(g);           // g_io
    assert.ok(menuA.length > 0, `np=${np}: dealt first-attacker has no legal moves`);

    await hammerReplay(code, input);                 // scribble the aliased replay bytes

    const menuB = kernelLegalMoves(g, pid);
    const stateB = serializeGameState(g);
    assert.deepEqual(menuB, menuA, `np=${np}: menu changed across an interleaved replay — overlay corruption`);
    assert.deepEqual([...stateB], [...stateA], `np=${np}: state marshal changed across an interleaved replay`);
  }
  assertEngineIsRules('at the end of the interleave');
});

// The tightest form: interleave the two families call-by-call and assert the
// menu never budges, and — after every replay call — a freshly dealt game still
// produces a well-formed snapshot/animation stream (covers proof obligation P1:
// the snapshot ring survives an aliased replay call).
test('R1 overlay: call-by-call interleave keeps menus stable and snapshots intact', async () => {
  const { code, input } = await tutorialReplay();

  const base = mkGame(4);
  start_game_packed(base);
  const basePid = base.players[base.first_attacker].player_id;
  const baseMenu = kernelLegalMoves(base, basePid);
  assert.ok(baseMenu.length > 0, 'base game has no opening menu');

  for (let k = 0; k < 6; k++) {
    await hammerReplay(code, input);

    // The resident base game's menu must still enumerate identically.
    assert.deepEqual(kernelLegalMoves(base, basePid), baseMenu, `menu diverged at interleave ${k}`);

    // A fresh deal immediately after a replay call must still fill the snapshot
    // ring. The kernel's own event count is the proof: wasm_events_serialize
    // builds the DEAL/FLIPPED stream out of g_snaps, so a corrupt ring shows up
    // as nEvents == 0.
    const g = mkGame(2 + (k % 5));
    const run = start_game_packed(g);
    assert.ok(run.nEvents > 0, `deal after replay ${k} produced no events (snapshot ring corrupt)`);
    for (const p of g.players) {
      assert.ok(p.hand.length > 0, `deal after replay ${k}: a player was dealt no cards`);
    }
    assert.ok(g.flipped && g.flipped.suit >= 0, `deal after replay ${k}: no trump flipped`);
    // And that fresh game's menu interleaves cleanly too.
    const gMenu = kernelLegalMoves(g, g.players[g.first_attacker].player_id);
    assert.ok(gMenu.length > 0, `deal after replay ${k}: first attacker has no legal moves`);
  }
  assertEngineIsRules('at the end of the interleave');
});
