/* =============================================================================
 * The packed pipeline's own guarantees: no leaks, and a stream that decodes.
 * =============================================================================
 * This file used to assert the C pipeline was BYTE-IDENTICAL to a TypeScript
 * one (handleX -> AnimationEvents -> the evwire TS encoder). That TypeScript
 * pipeline no longer exists: production runs every move through the kernel and
 * broadcasts the kernel's own per-viewer streams, so there is no second
 * implementation left to compare against and the parity half of this file went
 * with it.
 *
 * What was NOT about parity stays, because it is about the kernel's output on
 * its own terms:
 *   - the PERSONALIZATION invariant: a viewer's stream never carries another
 *     player's hand identities, and DEAL/REFILL identities reach only the
 *     receiving seat (assertNoLeaks, on the raw bytes);
 *   - a COVER event's target card and battle index agree - the two adjacent
 *     optional bytes a reader could take in either order;
 *   - the stream DECODES, and the game it decodes to mirrors the committed
 *     state;
 *   - the kernel's masked log export decodes to the right GameLog shapes;
 *   - an illegal wire is rejected.
 *
 * Pure kernel test - needs no Postgres (runs under VALIDATION_ONLY too).
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  Game, Card, AnimationEvent, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
  ANIMATION_EVENT_TYPE, PrivatePlayer,
} from '../server/api/core/types.ts';
import {
  serializeGameState, runPackedAction, kernelLegalMoves, kernelShouldAct, applyKernelStateToGame,
  __setKernelSeedSource,
} from '../sdk/ts/wasm/engine.ts';
import { start_game_packed } from '../server/api/common/game_lifecycle.ts';
import { encodeAction, AwireKindName } from '../sdk/ts/wire/awire.ts';
import { decodeEventWire } from '../sdk/ts/wire/evwire.ts';
import { kernelEventsFromPacked } from '../sdk/ts/wasm/bots.ts';
import { logsFromKernelExport, decodeLogs } from '../sdk/ts/wire/logwire.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// Deterministic RNG (same LCG as the fuzz suite) so failures reproduce.
let seed = Number(process.env.FUZZ_SEED || 0xbadc0de5) >>> 0;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);

// Both pipelines reseed the kernel once per action through this source;
// pinning one value per move makes their draws identical.
let moveSeed = 1;
__setKernelSeedSource(() => moveSeed);

const mkPlayer = (i: number, isAi: boolean): PrivatePlayer => ({
  player_id: `player-${i}`, name: `P${i}`, status: PLAYER_STATUS.READY,
  is_ai: isAi, hand: [], awaiting_attack: false, hand_length: 0,
  strategy_key: isAi ? STRATEGY_KEY.RANDOM : STRATEGY_KEY.HUMAN,
});

const mkLobby = (numPlayers: number): Game => ({
  id: 'parity', name: 'parity', status: GAME_STATUS.WAITING,
  players: Array.from({ length: numPlayers }, (_, i) => mkPlayer(i, i % 2 === 1)),
  deck: [], deck_length: 0, discard_pile_length: 0, flipped: null,
  power_suit: 0, first_attacker: 0, defender: 0, table_battles: [],
  elimination_order: [], good_timestamp: null, good_players: [], logs: [],
});

type Handler = (g: Game, pid: string) => AnimationEvent[];

// The check_win_sync + game-end event append executeWithGameLock performs
// after a winning handler (mirrored here so the JS-path stream includes the
// final MAGIC_TRANSITION the packed path emits via append_final_transition).


// Personalization scan: every event snapshot + the trailer of a viewer's stream
// must carry no non-viewer hand, and DEAL/REFILL card identities must reach only
// the receiving seat.
//
// This used to walk the bytes by hand with parseMaskedState. That parser is gone
// (A8/F7) and the kernel reads its own format now, so the scan asks the kernel —
// which is also the honest way round: it inspects what a CLIENT would actually
// be able to see, rather than what a second parser thinks is in there.
function assertNoLeaks(bytes: Uint8Array, viewer: number, numPlayers: number): void {
  assert.equal(bytes[0], 1, 'evwire format version');
  const seq = kernelEventsFromPacked(bytes);
  assert.equal(seq.viewer, viewer, 'the stream is addressed to this viewer');

  const noForeignHand = (state: { players: { seat: number; hand: unknown }[] }, where: string) => {
    for (const p of state.players) {
      if (p.seat === viewer) continue;
      assert.equal(p.hand, null, `seat ${p.seat} hand masked for viewer ${viewer} (${where})`);
    }
  };

  let checked = 0, covers = 0;
  for (const ev of seq.events) {
    // 1 = deal, 9 = refill (EVW_T_*); a card bound for someone else's hand.
    if ((ev.type === 1 || ev.type === 9) && ev.seat !== viewer) {
      for (const c of ev.cards) {
        assert.equal(c, null, `deal/refill cards masked (viewer ${viewer}, seat ${ev.seat})`);
      }
    }
    noForeignHand(ev.state, `event ${ev.type}`);

    // A COVER is the one event that carries BOTH optional trailer fields - the
    // covered attack card and the battle index it landed in (evwire.c's
    // ENGINE_HOOK_COVER emit). They are two adjacent single bytes behind one
    // flags byte, so a reader that takes them in the wrong order still decodes
    // the whole stream and still masks every hand: the leak scan above cannot
    // see it, and neither can a shape check. Tying them together is what makes
    // the order observable - the target must be the attack card sitting in the
    // battle the event names.
    if (ev.type === 5) {
      assert.ok(ev.target != null, 'a cover names the card it covered');
      assert.ok(ev.battle != null, 'a cover names the battle it landed in');
      const b = ev.state.battles[ev.battle!];
      assert.ok(b, `cover battle index ${ev.battle} is on the board (${ev.state.battles.length} battles)`);
      assert.deepEqual(
        b.attack, ev.target,
        `cover target ${JSON.stringify(ev.target)} is battle ${ev.battle}'s attack ${JSON.stringify(b.attack)}`,
      );
      covers++;
    }
    checked++;
  }
  noForeignHand(seq.game, 'trailer');
  assert.equal(checked, seq.events.length, 'every event was scanned');
  assert.ok(numPlayers >= 2);
  return covers;
}

test('every packed stream is leak-free, decodable, and mirrors the committed state', () => {
  const GAMES = Number(process.env.PARITY_GAMES || 24);
  let moves = 0, ends = 0, coversChecked = 0;

  for (let g = 0; g < GAMES; g++) {
    const numPlayers = 2 + (g % 4); // 2..5 players (36-card deck) - plus 6 below
    const game = mkLobby(g % 7 === 6 ? 6 : numPlayers);
    moveSeed = (g * 7919 + 13) >>> 0;
    start_game_packed(game);
    game.status = GAME_STATUS.PLAYING;

    const aiMask = game.players.reduce((m, p, i) => (p.is_ai ? m | (1 << i) : m), 0);
    const humanSeats = game.players.map((_, i) => i).filter(i => !game.players[i].is_ai);
    const roster = {
      id: game.id, name: game.name,
      players: game.players.map(p => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai })),
    };

    for (let mv = 0; mv < 600 && game.status === GAME_STATUS.PLAYING; mv++) {
      const eligible = game.players.filter(p => kernelShouldAct(game, p.player_id));
      if (eligible.length === 0) break;
      const actor = eligible[ri(eligible.length)];
      const menu = kernelLegalMoves(game, actor.player_id).filter(m => m.type !== 'wait');
      if (menu.length === 0) continue;
      const m = menu[ri(menu.length)];
      const kind = m.type as AwireKindName;
      const seat = game.players.findIndex(p => p.player_id === actor.player_id);

      const preGood = [...game.good_players];
      const preGoodTs = game.good_timestamp;
      moveSeed = (moveSeed * 48271 + mv + 1) >>> 0;

      const wire = encodeAction({ kind, cards: m.cards, attack_cards: m.attack_cards });
      const run = runPackedAction(serializeGameState(game), seat, wire, aiMask, humanSeats);
      assert.ok(run.ok, `packed path applied (${kind})`);
      if (!run.ok) continue;

      // The kernel's masked log export decodes to the right GameLog shapes -
      // DRAW identities stay hidden.
      const FIXED_TS = 1_700_000_000_000;
      const cLogBytes = logsFromKernelExport(run.logsWire, FIXED_TS);
      const decodedLogs = decodeLogs(cLogBytes, game.id, game.players);
      for (const dl of decodedLogs) {
        assert.ok(typeof dl.log_type === 'string' && dl.log_type.length > 0, 'log type decodes');
        for (const pair of dl.card_pairs) {
          assert.ok(pair.primary, 'a decoded pair names a primary card');
        }
      }

      // Advance the JS mirror to the kernel's post state, so the next move is
      // enumerated against the board the kernel just produced.
      applyKernelStateToGame(game, run.post, actor.player_id);

      for (const viewer of [...humanSeats, -1]) {
        const cBytes = run.events.get(viewer)!;
        coversChecked += assertNoLeaks(cBytes, viewer, game.players.length);

        const decoded = decodeEventWire(cBytes, roster, { preGood, prevGoodTs: preGoodTs, now: () => 4242 });
        assert.ok(decoded, 'stream decodes');
        // The decoded final game mirrors the committed public state.
        assert.equal(decoded!.game.status, game.status, 'status');
        assert.equal(decoded!.game.deck_length, game.deck.length, 'deck length');
        assert.equal(decoded!.game.discard_pile_length, game.discard_pile_length, 'discard length');
        assert.deepEqual(decoded!.game.good_players, game.good_players, 'good order reconstructed');
      }
      moves++;
      if (run.ended) { ends++; break; }
    }
  }

  assert.ok(moves > 100, `enough moves exercised (${moves})`);
  // A cover is the only event with both trailer bytes, so the check above is
  // only worth anything if covers actually happened.
  assert.ok(coversChecked > 100, `enough covers cross-checked (${coversChecked})`);
  console.error(`[packed-wire] moves=${moves} ends=${ends} covers=${coversChecked}`);

  // An illegal wire is rejected: the defender may not attack.
  {
    const game = mkLobby(3);
    moveSeed = 99;
    start_game_packed(game);
    game.status = GAME_STATUS.PLAYING;
    const defender = game.defender;
    const aiMask = game.players.reduce((m, p, i) => (p.is_ai ? m | (1 << i) : m), 0);
    const humanSeats = game.players.map((_, i) => i).filter(i => !game.players[i].is_ai);
    const card = game.players[defender].hand[0];
    const wire = encodeAction({ kind: 'attack', cards: [card] });
    const run = runPackedAction(serializeGameState(game), defender, wire, aiMask, humanSeats);
    assert.equal(run.ok, false, 'the defender attacking is rejected by the kernel');
  }
});
