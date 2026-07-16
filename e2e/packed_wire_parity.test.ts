// Packed-wire parity: the C kernel's one-call pipeline (wasm_apply_action +
// wasm_finalize_win + wasm_events_serialize + wasm_state_serialize) must be
// BYTE-IDENTICAL to the legacy JS path (handleX -> AnimationEvents ->
// wire/evwire.ts TS encoder; check_win_sync; serializeGameState) for every
// recipient of every move of whole seeded games. This is the guarantee that
// lets bot/meta broadcasts (TS-encoded) and human packed moves (C-encoded)
// interleave invisibly on the client.
//
// Also asserts the personalization invariant on the raw bytes: a viewer's
// stream never carries another player's hand identities (masked to 0xFE),
// and DEAL/REFILL card identities reach only the receiving seat.
//
// Pure kernel test — needs no Postgres (runs under VALIDATION_ONLY too).

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  Game, Card, AnimationEvent, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
  ANIMATION_EVENT_TYPE, PrivatePlayer,
} from '../supabase/functions/_shared/core/types.ts';
import {
  serializeGameState, runPackedAction, kernelLegalMoves, kernelShouldAct,
  __setKernelSeedSource,
} from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import { start_game } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { game_done } from '../supabase/functions/_shared/common/common_utils.ts';
import { handleAttack } from '../supabase/functions/_shared/common/actions/attack.ts';
import { handleCover } from '../supabase/functions/_shared/common/actions/cover.ts';
import { handlePass } from '../supabase/functions/_shared/common/actions/pass.ts';
import { handlePickup } from '../supabase/functions/_shared/common/actions/pickup.ts';
import { handleGood } from '../supabase/functions/_shared/common/actions/good.ts';
import { encodeAction, AwireKindName } from '../supabase/functions/_shared/sdk/ts/wire/awire.ts';
import { encodeEventWire, decodeEventWire } from '../supabase/functions/_shared/sdk/ts/wire/evwire.ts';
import { kernelEventsFromPacked } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { encodeLogs, logsFromKernelExport, decodeLogs } from '../supabase/functions/_shared/sdk/ts/wire/logwire.ts';

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
const dispatch = (g: Game, pid: string, kind: AwireKindName, cards?: Card[], attacks?: Card[]): AnimationEvent[] => {
  switch (kind) {
    case 'attack': return handleAttack(g, pid, cards!);
    case 'cover': return handleCover(g, pid, cards!, attacks!);
    case 'pass': return handlePass(g, pid, cards!);
    case 'pickup': return handlePickup(g, pid);
    case 'good': return handleGood(g, pid);
  }
};

// The check_win_sync + game-end event append executeWithGameLock performs
// after a winning handler (mirrored here so the JS-path stream includes the
// final MAGIC_TRANSITION the packed path emits via append_final_transition).
const finalizeJs = (g: Game, events: AnimationEvent[]): boolean => {
  if (game_done(g) === null) return false;
  g.status = GAME_STATUS.GAME_OVER;
  for (const p of g.players) p.status = p.is_ai ? PLAYER_STATUS.READY : PLAYER_STATUS.IDLE;
  events.push({ type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION, game_state: g } as AnimationEvent);
  return true;
};

const hexDiff = (a: Uint8Array, b: Uint8Array): string => {
  if (a.length !== b.length) return `length ${a.length} != ${b.length}`;
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) return `byte ${i}: ${a[i]} != ${b[i]}`;
  }
  return 'equal';
};

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

  let checked = 0;
  for (const ev of seq.events) {
    // 1 = deal, 9 = refill (EVW_T_*); a card bound for someone else's hand.
    if ((ev.type === 1 || ev.type === 9) && ev.seat !== viewer) {
      for (const c of ev.cards) {
        assert.equal(c, null, `deal/refill cards masked (viewer ${viewer}, seat ${ev.seat})`);
      }
    }
    noForeignHand(ev.state, `event ${ev.type}`);
    checked++;
  }
  noForeignHand(seq.game, 'trailer');
  assert.equal(checked, seq.events.length, 'every event was scanned');
  assert.ok(numPlayers >= 2);
}

test('packed C pipeline is byte-identical to the JS path + TS encoder across seeded games', () => {
  const GAMES = Number(process.env.PARITY_GAMES || 24);
  let moves = 0, ends = 0, rejects = 0;

  for (let g = 0; g < GAMES; g++) {
    const numPlayers = 2 + (g % 4); // 2..5 players (36-card deck) — plus 6 below
    const game = mkLobby(g % 7 === 6 ? 6 : numPlayers);
    moveSeed = (g * 7919 + 13) >>> 0;
    start_game(game);
    game.status = GAME_STATUS.PLAYING;

    const aiMask = game.players.reduce((m, p, i) => (p.is_ai ? m | (1 << i) : m), 0);
    const humanSeats = game.players.map((_, i) => i).filter(i => !game.players[i].is_ai);
    const roster = {
      id: game.id, name: game.name,
      players: game.players.map(p => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai })),
    };

    for (let mv = 0; mv < 600 && game.status === GAME_STATUS.PLAYING; mv++) {
      // Pick an eligible actor + a legal move, fuzz-style.
      const eligible = game.players.filter(p => kernelShouldAct(game, p.player_id));
      if (eligible.length === 0) break;
      const actor = eligible[ri(eligible.length)];
      const menu = kernelLegalMoves(game, actor.player_id).filter(m => m.type !== 'wait');
      if (menu.length === 0) continue;
      const m = menu[ri(menu.length)];
      const kind = m.type as AwireKindName;
      const seat = game.players.findIndex(p => p.player_id === actor.player_id);

      const pre = structuredClone(game);
      const preGood = [...game.good_players];
      const preLogCount = game.logs.length;
      moveSeed = (moveSeed * 48271 + mv + 1) >>> 0;

      // JS path (mutates `game`).
      const events = dispatch(game, actor.player_id, kind, m.cards, m.attack_cards);
      const ended = finalizeJs(game, events);

      // Packed path from the identical pre-state + identical kernel seed.
      const wire = encodeAction({ kind, cards: m.cards, attack_cards: m.attack_cards });
      const run = runPackedAction(serializeGameState(pre), seat, wire, aiMask, humanSeats);
      assert.ok(run.ok, `packed path applied (${kind})`);
      if (!run.ok) continue;
      assert.equal(run.ended, ended, 'both paths agree on game end');

      // Durable state: byte-identical.
      const jsBlob = serializeGameState(game);
      assert.equal(hexDiff(run.stateBlob, jsBlob), 'equal',
        `state blob parity (game ${g} move ${mv} ${kind})`);

      // Session-log records: the kernel's DRAW-masked export must be
      // byte-identical to the JS path's appendLogs output (which hides the
      // same identities) once encoded through logwire with pinned clocks.
      const FIXED_TS = 1_700_000_000_000;
      const newLogs = game.logs.slice(preLogCount).map(l => ({ ...l, created_at: new Date(FIXED_TS).toISOString() }));
      const seatOfPid = (pid: string | null) => pid === null ? -1 : game.players.findIndex(p => p.player_id === pid);
      const jsLogBytes = encodeLogs(newLogs, seatOfPid);
      const cLogBytes = logsFromKernelExport(run.logsWire, FIXED_TS);
      assert.equal(hexDiff(cLogBytes, jsLogBytes), 'equal',
        `logwire parity (game ${g} move ${mv} ${kind})`);
      // ...and the decode round-trips to the same GameLog shapes.
      const decodedLogs = decodeLogs(cLogBytes, game.id, game.players);
      assert.equal(decodedLogs.length, newLogs.length, 'decoded log count');
      decodedLogs.forEach((dl, i) => {
        assert.equal(dl.log_type, newLogs[i].log_type, 'log type');
        assert.equal(dl.player_id, newLogs[i].player_id, 'log player');
        assert.deepEqual(dl.card_pairs, newLogs[i].card_pairs.map(p => ({ primary: p.primary, target: p.target ?? null })), 'log pairs (draws hidden)');
      });

      // Event streams: byte-identical per recipient, leak-free, decodable.
      for (const viewer of [...humanSeats, -1]) {
        const tsBytes = encodeEventWire(events, game, viewer, seat);
        const cBytes = run.events.get(viewer)!;
        assert.equal(hexDiff(cBytes, tsBytes), 'equal',
          `event wire parity (game ${g} move ${mv} ${kind} viewer ${viewer})`);
        assertNoLeaks(cBytes, viewer, game.players.length);

        const decoded = decodeEventWire(cBytes, roster, { preGood, prevGoodTs: pre.good_timestamp, now: () => 4242 });
        assert.ok(decoded, 'stream decodes');
        assert.equal(decoded!.events.length, events.length, 'decoded event count matches JS events');
        decoded!.events.forEach((de, i) => {
          assert.equal(de.type, events[i].type, 'event type');
          assert.equal(de.player_id, events[i].player_id, 'event player');
          assert.equal(de.message, events[i].message, 'reconstructed message matches the JS template');
          assert.equal(de.battle_index, events[i].battle_index, 'battle index');
        });
        // The decoded final game mirrors the committed public state.
        assert.equal(decoded!.game.status, game.status);
        assert.equal(decoded!.game.deck_length, game.deck.length);
        assert.equal(decoded!.game.discard_pile_length, game.discard_pile_length);
        assert.deepEqual(decoded!.game.good_players, game.good_players, 'good order reconstructed');
      }
      moves++;
      if (ended) { ends++; break; }
    }
  }

  // Reject parity spot-check: an illegal wire rejects in both pipelines.
  {
    const game = mkLobby(3);
    moveSeed = 99;
    start_game(game);
    game.status = GAME_STATUS.PLAYING;
    const defender = game.defender;
    const pid = game.players[defender].player_id;
    // The defender attacking is illegal.
    const card = game.players[defender].hand[0];
    assert.throws(() => handleAttack(structuredClone(game), pid, [card]), /defender/i);
    const run = runPackedAction(
      serializeGameState(game), defender,
      encodeAction({ kind: 'attack', cards: [card] }), 0, [0, 1, 2]);
    assert.ok(!run.ok, 'packed path rejects too');
    if (!run.ok) assert.ok(run.reason > 0, 'carries a reject code');
    rejects++;
  }

  assert.ok(moves > 300, `exercised enough moves (${moves})`);
  assert.ok(ends > 3, `enough games played to completion (${ends})`);
  console.log(`parity: ${moves} moves, ${ends} completed games, ${rejects} reject checks`);
});
