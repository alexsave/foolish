/* =============================================================================
 * Replay codec property test
 * =============================================================================
 * Plays random legal games with the REAL server engine (the same
 * handleAttack/handleCover/... the edge functions run), then:
 *
 *   game.logs -> encodeReplay -> bigint -> bytes -> base32/base64
 *            -> decodeReplay (public-state replayer, decode direction)
 *
 * and asserts the decoded stream reproduces the ENTIRE original log stream —
 * not just the coded actions but every derived DISCARD / DRAW /
 * DEFENDER_CHANGE / PLAYER_OUT, byte for byte. Any rules drift between the
 * server engine and _shared/common/replay/core.ts shows up here as a hard failure.
 *
 * This is a pure codec/engine test — no Postgres, no harness.
 * Games-per-player-count is REPLAY_GAMES_PER_PC (default 20).
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { game_done } from '../server/api/common/common_utils.ts';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import {
  Game,
  GameLog,
  GAME_STATUS,
  PLAYER_STATUS,
  PrivatePlayer,
  StrategyKey,
  LOG_TYPE,
} from '../server/api/core/types.ts';
import { shouldBotActCore, processBotAction } from '../server/api/common/pure_bot_actions.ts';
import { calculateLegalMoves } from '../server/api/common/bot_strategy.ts';
import { ReplayInput, SeatLog, DecodedReplay, INFO_TYPES } from '../server/api/common/replay/core.ts';
import { decodeReplay } from '../server/api/common/replay/decode.ts';
import {
  urlToGame, base64Decode, base64Encode, base32Encode, bytesToBigint, codeToGame, gameToUrl,
} from '../server/api/common/replay/codec.ts';
import { kernelReplayEncodeV6FromGame } from '../sdk/ts/wasm/bots.ts';
import { __setDealSeedOverride, isReplayTooLong } from '../sdk/ts/wasm/engine.ts';

const hexToBytes = (h: string) => Uint8Array.from(h.match(/../g)!.map((b) => parseInt(b, 16)));
import {
  encodeExtras,
  encodeExtrasFromGaps,
  decodeExtras,
  splitReplayCode,
  joinReplayCode,
  moveTimesFromLogs,
} from '../server/api/common/replay/extras.ts';
import { buildReplayFrames, REPLAY_STEP } from '../src/replay/frames';

// The engine logs play-by-play; silence it so the test reporter stays readable.
if (!process.env.E2E_VERBOSE) {
  console.log = () => {};
  console.warn = () => {};
  console.error = () => {};
  console.info = () => {};
}

const GAMES_PER_PC = Number(process.env.REPLAY_GAMES_PER_PC ?? 20);
const MAX_ACTIONS = 100000;

/* ---- the run seed -------------------------------------------------------
 * Which bot acts first at a shared state used to come from Math.random, so
 * every run played 140 DIFFERENT games and a red one was not re-runnable. It
 * cost a day: the coverage job on main went red on a game nobody could play
 * again (`replay: malformed encode input`, ~0.1% of the `random`-bot games).
 *
 * So the stream is the kernel's own strategy LCG (c/src/game.c
 * random_strategy_random), seeded from the environment. CI plays the same
 * games every run; a human widens the search with REPLAY_CODEC_SEED=<n>.
 *
 * The DEFAULT is chosen for what it covers, not for passing: seed 6 reaches the
 * long-game refusal below (an 8-player `random` game of 1077 logs), so the path
 * that broke CI is exercised on every run instead of once a fortnight.
 */
const CODEC_SEED = Number(process.env.REPLAY_CODEC_SEED ?? 6);
const REPRO = `REPLAY_CODEC_SEED=${CODEC_SEED} REPLAY_GAMES_PER_PC=${GAMES_PER_PC}`;
const mkLcg = (seed: number) => {
  let s = (seed >>> 0) || 1;
  return () => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    return s / 4294967296;
  };
};
/** Pin Math.random for the body, and put the real one back even if it throws. */
async function seeded<T>(fn: () => Promise<T>): Promise<T> {
  const real = Math.random;
  Math.random = mkLcg(CODEC_SEED);
  try { return await fn(); } finally { Math.random = real; }
}

/* c/src/game.h MAX_LOGS. A game that reaches it stopped being recorded (the
 * remaining logs were dropped), so v6 refuses it outright (REPLAY_ETOOLONG) and
 * that game has no code at all: the owner's call, documented in replay.h, and
 * the same skip c/tests/replay_difftest.c and c/ios/ios_api_smoke.c take.
 *
 * The harness checks the ceiling ITSELF rather than catching the refusal,
 * because a caught refusal is a blank cheque: any encoder fault that happened
 * to be reported as the same code would be skipped forever. Here only games
 * this test can SEE are over the cap get skipped, and every other throw is a
 * failure. If the two ever disagree, roundTripGame says so by name.
 *
 * MUTATION-CHECKED: `&& false` on the predicate below (i.e. no skip at all)
 * reproduces the original red job exactly - "v6 encode failed on a 1077-log 8p
 * game (np=8 game=10 strategy=random, REPLAY_CODEC_SEED=6 ...)" - and reports
 * the same game on every re-run, which is the point of the seed. */
const KERNEL_MAX_LOGS = 1024;
const overranTheLogBuffer = (game: Game) => game.logs.length >= KERNEL_MAX_LOGS;

const mkPlayer = (i: number, strategy: StrategyKey): PrivatePlayer => ({
  player_id: `bot_${i}`,
  name: `Bot ${i}`,
  status: PLAYER_STATUS.READY,
  is_ai: true,
  hand: [],
  awaiting_attack: false,
  hand_length: 0,
  strategy_key: strategy,
});

const mkGame = (np: number, strategy: StrategyKey): Game => ({
  players: Array.from({ length: np }, (_, i) => mkPlayer(i, strategy)),
  deck: [],
  logs: [],
  id: 'g',
  name: 'g',
  status: GAME_STATUS.PLAYING,
  deck_length: 0,
  discard_pile_length: 0,
  flipped: null,
  power_suit: 0,
  first_attacker: 0,
  defender: 0,
  table_battles: [],
  elimination_order: [],
  good_timestamp: null,
  good_players: [],
});

// Seeded, because the v6 producer re-derives the deal FROM the seed. Without
// the override the TS engine deals from its own RNG and the kernel rebuilds a
// different game ("logged attack not in menu").
let seedCounter = 0;
async function playRandomGame(np: number, strategy: StrategyKey): Promise<Game | null> {
  const game = mkGame(np, strategy);
  const seed = Array.from({ length: 32 },
    (_, i) => ((i * 53 + (++seedCounter) * 89 + np * 13) & 0xff).toString(16).padStart(2, '0')).join('');
  __setDealSeedOverride(hexToBytes(seed));
  try {
    start_game(game);
  } finally {
    __setDealSeedOverride(null);
  }
  let actions = 0;
  while (game_done(game) === null) {
    if (++actions > MAX_ACTIONS) return null; // stalled, skip (harness issue)
    const eligible: PrivatePlayer[] = [];
    for (let i = 0; i < game.players.length; i++) {
      const p = game.players[i];
      if (shouldBotActCore(game, p, i) && calculateLegalMoves(game, p.player_id).length > 0) {
        eligible.push(p);
      }
    }
    if (eligible.length === 0) return null;
    const order = [...eligible];
    for (let i = order.length - 1; i > 0; i--) {
      const j = Math.floor(Math.random() * (i + 1));
      [order[i], order[j]] = [order[j], order[i]];
    }
    let acted = false;
    for (const p of order) {
      if (await processBotAction(game, p)) {
        acted = true;
        break;
      }
    }
    if (!acted) return null;
  }
  return game;
}

/* normalize both streams to a comparable shape. GOOD presses are implied in
 * format v4 — the decoder neither stores nor reproduces them — so they are
 * stripped from the original before comparing. Everything else (including
 * every derived DISCARD/DRAW/DEFENDER_CHANGE/PLAYER_OUT) must match. */
function normOriginal(game: Game): SeatLog[] {
  const seatOf = (pid: string | null) =>
    pid === null ? null : game.players.findIndex((p) => p.player_id === pid);
  return game.logs
    .filter((l) => l.log_type !== LOG_TYPE.GOOD)
    .map((l: GameLog) => ({
      log_type: l.log_type,
      seat: seatOf(l.player_id),
      card_pairs: l.card_pairs.map((p) => ({
        primary: { suit: p.primary.suit, value: p.primary.value },
        target: p.target ? { suit: p.target.suit, value: p.target.value } : null,
      })),
      defender_index: l.defender_index ?? null,
    }));
}

function normDecoded(d: DecodedReplay): SeatLog[] {
  return d.logs
    .filter((l) => l.log_type !== LOG_TYPE.GOOD)
    .map((l) => ({
      log_type: l.log_type,
      seat: l.seat,
      card_pairs: l.card_pairs.map((p) => ({
        primary: { suit: p.primary.suit, value: p.primary.value },
        target: p.target ? { suit: p.target.suit, value: p.target.value } : null,
      })),
      defender_index: l.defender_index ?? null,
    }));
}

/** First differing entry between two streams, or null if identical. */
function diffStreams(a: SeatLog[], b: SeatLog[]): string | null {
  const ja = a.map((l) => JSON.stringify(l));
  const jb = b.map((l) => JSON.stringify(l));
  const len = Math.max(ja.length, jb.length);
  for (let i = 0; i < len; i++) {
    if (ja[i] !== jb[i]) {
      return `entry ${i}:\n  original: ${ja[i] ?? '<end>'}\n  decoded:  ${jb[i] ?? '<end>'}`;
    }
  }
  return null;
}

// Full encode -> serialize -> decode -> verify pipeline for one finished game.
//
// Through the PRODUCTION producer. This used to run the v5 encoder, which is
// gone: v5 hid the deal, so its replays retrodicted hands, and the only thing it
// bought was covering the games v6 refuses — which trimming the dead goods took
// to 0.05% of 8-player games. A game longer than that gets no code at all
// (owner's call), so there is nothing left for a second encoder to do.
async function roundTripGame(game: Game, np: number, where: string): Promise<void> {
  let bytes: Uint8Array;
  try {
    bytes = kernelReplayEncodeV6FromGame(game, hexToBytes(game.game_seed!));
  } catch (e) {
    // The one refusal that is not a fault, reached anyway: the caller's ceiling
    // check let a game through that the kernel calls too long, so the two
    // disagree about MAX_LOGS. Say that, rather than let the next reader read
    // it as a codec bug.
    if (isReplayTooLong(e)) {
      throw new Error(`KERNEL_MAX_LOGS (${KERNEL_MAX_LOGS}) no longer matches c/src/game.h MAX_LOGS: `
        + `the kernel refused a ${game.logs.length}-log ${np}p game as too long (${where}, ${REPRO})`);
    }
    throw new Error(`v6 encode failed on a ${game.logs.length}-log ${np}p game `
      + `(${where}, ${REPRO}): ${(e as Error).message}`);
  }
  const x = bytesToBigint(bytes);
  const enc = {
    x, bytes, byteLength: bytes.length,
    base32: base32Encode(bytes), base64: base64Encode(bytes), url: gameToUrl(x),
  };

  // decode through every serialization layer
  const xUrl = urlToGame(enc.url);
  const xB64 = bytesToBigint(base64Decode(enc.base64));
  assert.equal(xUrl, enc.x, 'serialization round-trip mismatch (url)');
  assert.equal(xB64, enc.x, 'serialization round-trip mismatch (base64)');

  // The decode is the game the engine actually played — compared on the INFO
  // actions, which is the honest common denominator between the two streams.
  //
  // It used to compare the FULL stream, which only worked against v5: v5 hid
  // the deal, so its decode reproduced exactly the masked stream it was fed. v6
  // is hidden-state-lossless, so its decode legitimately carries MORE than the
  // TS original — a per-seat deal DRAW the TS engine never logs, and every later
  // DRAW resolved to a real card where the original masks it. That is the fix,
  // not a drift. The full stream's fidelity is asserted where the truth lives:
  // c/tests/replay_v6_test.c holds the decode against the ENGINE's own deal
  // and draws, which no TS-side comparison can reach.
  const dec = await decodeReplay(enc.x);
  const infoOf = (ls: SeatLog[]) => ls.filter((l) => INFO_TYPES.includes(l.log_type));
  assert.equal(diffStreams(infoOf(normOriginal(game)), infoOf(normDecoded(dec))), null,
    'info-action stream mismatch');

  // elimination order / fool / discard must match too
  const elim = game.elimination_order.map((pid) => game.players.findIndex((p) => p.player_id === pid));
  assert.deepEqual(elim, dec.eliminationOrder, 'elimination mismatch');
  const fool = game.players.findIndex((p) => p.player_id === game_done(game));
  assert.equal(fool, dec.fool, 'fool mismatch');
  assert.equal(game.discard_pile_length, dec.discardPileLength, 'discard pile mismatch');

  // The replay screen must be able to replay this code. It used to fold the log
  // stream into boards itself, and this asserted the fold's arithmetic (one step
  // per log, plus a synthetic end). There is no fold now — the screen renders the
  // frames the engine produces — so what is worth asserting is that the code
  // replays at all, and lands on the game that was played.
  const frames = buildReplayFrames(enc.bytes, 'g', null, { fool: dec.fool });
  assert.ok(frames.length > 1, 'the code replays to steps');
  const lastFrame = frames[frames.length - 1];
  lastFrame.game.players.forEach((p, s) => {
    // The engine's own out-flags: at game end everyone but the fool is out,
    // including the seats emptied by a refill, which log nothing.
    assert.equal(p.status === PLAYER_STATUS.OUT, s !== dec.fool, `out-flag mismatch at seat ${s}`);
    assert.equal(p.hand_length, game.players[s].hand.length, `seat ${s} ends on its real hand size`);
  });

  // extras (names + per-move timing) round-trip
  const unicodeNames = [
    'ВАСЯ \u{1F0CF}',
    '한국이름',
    'ÉMILIE',
    'P4',
    'X'.repeat(60), // over the byte cap, must trim cleanly
    'P6',
    'P7',
    'P8',
  ].slice(0, np);
  let t = 1750000000;
  game.logs.forEach((l, idx) => {
    t += [0.05, 0.4, 2, 9, 45, 600, 90000][idx % 7];
    l.created_at = new Date(t * 1000).toISOString();
  });
  const moveTimes = moveTimesFromLogs(game.logs);
  const extras = encodeExtras(unicodeNames, moveTimes);
  const full = joinReplayCode(enc.base32, extras);
  const { moves: m2, extras: x2 } = splitReplayCode(full);
  assert.equal(m2, enc.base32, 'extras container split mismatch (moves)');
  assert.equal(x2, extras, 'extras container split mismatch (extras)');
  assert.equal(urlToGame('WWW.FOOLISH.CARDS/' + full), enc.x, 'urlToGame must ignore the extras section');

  const moveCount = dec.logs.filter((l) => INFO_TYPES.includes(l.log_type)).length;
  assert.equal(moveCount, moveTimes.length - 1, 'extras: move count vs times mismatch');
  const back = decodeExtras(extras, np, moveCount);
  back.names!.forEach((nm, i) => {
    const orig = unicodeNames[i];
    assert.ok(nm === orig || orig.startsWith(nm), `name mismatch: ${nm} vs ${orig}`);
  });
  assert.ok(Math.abs(back.startTime! - Math.floor(moveTimes[0])) <= 1, 'start time mismatch');
  back.moveGaps!.forEach((g, i) => {
    const want = moveTimes[i + 1] - moveTimes[i];
    assert.ok(Math.abs(g - want) <= Math.max(0.08, want * 0.08), `gap ${i}: got ${g}, want ${want}`);
  });

  // The bout leader must be the seat that actually opens each bout. This used
  // to check the view's own DERIVATION of it (a pass moves the defender without
  // moving the leader, and the fold had to mirror that rule by hand). Nothing
  // derives it now — first_attacker is on the board the engine committed — so
  // what is left to check is that the board agrees with what happened.
  for (let s = 1; s < frames.length; s++) {
    if (
      frames[s].kind === REPLAY_STEP.ATTACK &&
      frames[s - 1].game.table_battles.length === 0 &&
      frames[s].seat !== frames[s - 1].game.first_attacker
    ) {
      throw new Error(
        `firstAttacker drift at step ${s}: P${frames[s].seat} opened, board says P${frames[s - 1].game.first_attacker}`,
      );
    }
  }
}

// Owns the replay validation scenarios; the fast runner
// (e2e/validation/replay_validation.test.ts) imports `registerReplayValidation`.
export function registerReplayValidation(): void {
  // The tutorial's own frozen code used to be guarded here. It now has a suite
  // of its own — e2e/tutorial_game.test.ts, which replays it rather than just
  // decoding it — registered into this same fast runner by
  // e2e/validation/tutorial_validation.test.ts.
  test('kernel decode rejects garbage and future versions cleanly', async () => {
    // version 7 header: the smallest integer whose version field is neither the
    // frozen v5 nor the additive v6 (both are now supported); 7 stays unknown.
    await assert.rejects(
      () => decodeReplay(7n),
      /unsupported replay format version 7/,
    );
    // random bytes: must terminate — either a clean throw or (by chance) a
    // well-formed decode, never a hang or a malformed structure
    const junk = new Uint8Array(64);
    for (let i = 0; i < junk.length; i++) junk[i] = (i * 37 + 11) & 0xff;
    try {
      const d = await decodeReplay(bytesToBigint(junk));
      assert.ok(Array.isArray(d.logs));
    } catch {
      // expected: a clean rejection
    }
  });
  // Scale-free timing self-test: the same 1-byte/move curve must hold from
  // nanosecond simulation steps to multi-week correspondence gaps.
  test('replay extras: time scale holds from 1ns to 1 week units', () => {
    for (const scale of [1e-9, 1e-6, 1e-3, 1, 3600, 86400 * 7]) {
      const t0 = 1750000000;
      const raw = [1, 2.5, 7, 0.3, 40, 12, 0.9, 100];
      const gaps = raw.map((r) => r * scale);
      const blob = encodeExtrasFromGaps(null, t0, gaps);
      const back = decodeExtras(blob, 2, raw.length);
      back.moveGaps!.forEach((g, i) => {
        const want = raw[i] * scale;
        assert.ok(Math.abs(g - want) <= want * 0.08, `time scale ${scale}: gap ${i} got ${g}, want ${want}`);
      });
    }
  });

  // A few short engine games round-trip byte-exact (engine<->replay drift guard).
  test('replay codec round-trips short engine games byte-exact (2..4 players)', async () => {
    let played = 0;
    await seeded(async () => {
      for (let np = 2; np <= 4; np++) {
        for (let g = 0; g < 2; g++) {
          const strategy = (g % 2 === 0 ? 'random' : 'handwritten') as StrategyKey;
          const game = await playRandomGame(np, strategy);
          if (!game || overranTheLogBuffer(game)) continue;
          played++;
          await roundTripGame(game, np, `np=${np} game=${g} strategy=${strategy}`);
        }
      }
    });
    assert.ok(played > 0, `at least one short game completed (${REPRO})`);
  });
}

if (!process.env.VALIDATION_ONLY) registerReplayValidation();

if (!process.env.VALIDATION_ONLY) test(`replay codec round-trips engine-played games (${GAMES_PER_PC}/player-count, 2..8 players)`, async () => {
  let totalGames = 0;
  let stalled = 0;
  let tooLong = 0;
  process.stderr.write(`[replay_codec] ${REPRO}\n`);
  await seeded(async () => {
    for (let np = 2; np <= 8; np++) {
      for (let g = 0; g < GAMES_PER_PC; g++) {
        // Alternate fast strategies for distribution diversity: random explores
        // the legal-move space (correctness), handwritten produces realistic game
        // shapes. (cordite is omitted - its MCTS rollouts are far too slow for a
        // codec test, and add no coverage the log stream doesn't already get.)
        const strategy = (g % 2 === 0 ? 'random' : 'handwritten') as StrategyKey;
        const game = await playRandomGame(np, strategy);
        if (!game) {
          stalled++;
          continue;
        }
        // The build's ceiling, not a codec fault: this game was never fully
        // recorded, so it has no v6 code. `random` bots at 7-8 seats reach it
        // about once per thousand games.
        if (overranTheLogBuffer(game)) {
          tooLong++;
          continue;
        }
        totalGames++;
        await roundTripGame(game, np, `np=${np} game=${g} strategy=${strategy}`);
      }
    }
  });
  assert.ok(totalGames > 0, `no games completed (stalled=${stalled}, ${REPRO})`);
  // A ceiling that swallowed the suite would otherwise pass silently.
  assert.ok(tooLong <= totalGames * 0.05,
    `${tooLong} of ${totalGames + tooLong} games overran the log buffer: `
    + `the harness is skipping its own coverage (${REPRO})`);
});
