/* =============================================================================
 * Replay codec property test
 * =============================================================================
 * Plays random games the way the server plays them - a C Table dealt from a
 * seed and driven by the kernel's own bot cycle (helpers/bot_table.ts) - then:
 *
 *   state + session log + deal seed -> table_replay_code (the finalize path's
 *   encoder) -> bytes -> base32 / base64 / link -> back
 *
 * and asserts the decoded code reproduces every information-bearing action of
 * the game that was played, in order and by seat, plus its fool, its
 * elimination order, its trump and its closing board. The truth is what the
 * table itself sent and served: its live pushes and its closing envelopes
 * (helpers/replay_play.ts). Any rules drift between the engine and the kernel's
 * replay projection (c/src/replay.c, replay_steps.c) shows up here as a hard
 * failure.
 *
 * table_replay_code also refuses a code whose decode does not reproduce the
 * session log's attack, cover, pass and pickup records (its round-trip gate, in
 * C); this file holds the decode to the game from the other side, through what
 * a client was shown.
 *
 * This is a pure codec/engine test - no Postgres, no harness.
 * Games-per-player-count is REPLAY_GAMES_PER_PC (default 20).
 * ========================================================================== */

import { test } from 'node:test';
import { bytesToBase64, base64ToBytes } from '../sdk/ts/wire/bytes.ts';
import assert from 'node:assert/strict';
import {
  kernelB32Encode, kernelReplayLink, kernelB32Decode,
  replayPlaySteps, replaySummary,
} from '../sdk/ts/wasm/bots.ts';
import { kernelReplayLinkParse } from './helpers/test_kernel_doors.ts';
import { REPLAY_ATOM_ATTACK, REPLAY_ATOM_COVER, REPLAY_ATOM_PASS, REPLAY_ATOM_PICKUP } from '../sdk/ts/gen/kernel_consts.bots.ts';
import * as V from '../sdk/ts/gen/view_layout.bots.ts';
import { REPLAY_ETOOLONG } from '../sdk/ts/gen/game_layout.bots.ts';
import {
  bytesToBigint,
} from '../server/api/common/replay_codec.ts';
import {
  decodeExtras,
  splitReplayCode,
  joinReplayCode,
} from '../server/api/common/replay_extras.ts';
import { buildReplayFrames } from '../src/replay/frames';
import { fixtureTable } from './helpers/table_fixture.ts';
import { recordBotTable } from './helpers/replay_play.ts';
import { suiteRng } from './helpers/rng.ts';

// A pasted link as the moves bigint: the kernel strips and refuses
// (replay_link_parse), the kernel decodes (replay_b32_decode). Composed here
// rather than in the product, which never needed the bigint form.
const urlToGame = (url: string): bigint =>
    bytesToBigint(kernelB32Decode(kernelReplayLinkParse(url)));

// The engine logs play-by-play; silence it so the test reporter stays readable.
if (!process.env.E2E_VERBOSE) {
  console.log = () => {};
  console.warn = () => {};
  console.error = () => {};
  console.info = () => {};
}

const GAMES_PER_PC = Number(process.env.REPLAY_GAMES_PER_PC ?? 20);

// The deal is pinned per game (seedCounter below), and every bot decision is a
// function of the board and the deal seed; the suite seed is mixed into the
// deals, so sweeping E2E_SEED_REPLAY_CODEC sweeps the games. Same games per
// seed, every run.
const rng = suiteRng('replay_codec');

// The names a game is played under: every width, script and the byte cap.
const NAMES = [
  'ВАСЯ \u{1F0CF}',
  '한국이름',
  'ÉMILIE',
  'P4',
  'X'.repeat(60), // over the byte cap, must trim cleanly
  'P6',
  'P7',
  'P8',
];

let seedCounter = 0;
type Played = ReturnType<typeof recordBotTable>;
function playRandomGame(np: number, brain: string): Played {
  const salt = rng.int(256);
  const seed = Uint8Array.from({ length: 32 },
    (_, i) => ((i * 53 + (++seedCounter) * 89 + np * 13) ^ salt) & 0xff);
  return recordBotTable(Array(np).fill(brain), seed, { names: NAMES.slice(0, np) });
}

/** The finalize path's encode of a finished game: its code, or the kernel's refusal code. */
function encodeGame(game: Pick<Played, 'state' | 'roster' | 'seed' | 'log'>): Uint8Array | number {
  const table = fixtureTable();
  const rc = table.load(game.state, game.roster);
  if (rc < 0) throw new Error(`the finished game does not load (${rc})`);
  return table.replayCode(game.seed, game.log);
}

const tooLong = (rc: number) => rc === -REPLAY_ETOOLONG;

const INFO_STEP: Record<number, string> = {
  [REPLAY_ATOM_ATTACK]: 'attack', [REPLAY_ATOM_PASS]: 'pass',
  [REPLAY_ATOM_COVER]: 'cover', [REPLAY_ATOM_PICKUP]: 'pickup',
};

/** The information-bearing moves live play pushed, in order: kind and acting seat. */
function pushedMoves(game: Played): string[] {
  const out: string[] = [];
  for (const e of game.events) {
    const kind = e.type === V.EVW_T_ATTACK_PASS ? (e.msg === V.EVW_MSG_PASSED ? 'pass' : 'attack')
      : e.type === V.EVW_T_COVER ? 'cover' : e.type === V.EVW_T_PICKUP ? 'pickup' : null;
    if (kind) out.push(`${kind}@${e.seat}`);
  }
  return out;
}

/** First differing entry between two streams, or null if identical. */
function diffStreams(a: string[], b: string[]): string | null {
  const len = Math.max(a.length, b.length);
  for (let i = 0; i < len; i++) {
    if (a[i] !== b[i]) return `entry ${i}:\n  played:  ${a[i] ?? '<end>'}\n  decoded: ${b[i] ?? '<end>'}`;
  }
  return null;
}

// Full encode -> serialize -> decode -> verify pipeline for one finished game.
//
// Returns false for the one outcome that is a REFUSAL and not a failure: a game
// whose session log outran the kernel's MAX_LOGS gets no code at all, by build
// design. The KERNEL decides that, by its own MAX_LOGS; this file carries no
// copy of that build parameter to guess with. Every other encode error is a
// fault and throws.
function roundTripGame(game: Played, np: number, where: string): boolean {
  const bytes = encodeGame(game);
  if (typeof bytes === 'number') {
    // The documented refusal (REPLAY_ETOOLONG), reported by the kernel itself.
    // Not an assertion failure: there is nothing to round-trip.
    if (tooLong(bytes)) return false;
    throw new Error(`v6 encode failed on a ${np}p game (${where}, seed=${rng.seed}): kernel refusal ${bytes}`);
  }
  const x = bytesToBigint(bytes);
  const enc = {
    x, bytes,
    base32: kernelB32Encode(bytes), base64: bytesToBase64(bytes),
    url: kernelReplayLink(kernelB32Encode(bytes), []),
  };

  // decode through every serialization layer
  const xUrl = urlToGame(enc.url);
  // ...including the link a person actually pastes. A browser hands out
  // https://foolish.cards/<code>, never the printed WWW.FOOLISH.CARDS/ form,
  // and every letter of `https` is in the base32 alphabet - so an unrecognised
  // prefix does not fail, it names a DIFFERENT game, which then dies in the
  // kernel under a codec error that was never the fault.
  const bareCode = enc.base32;
  for (const pasted of [
    `https://foolish.cards/${bareCode}`,
    `https://www.foolish.cards/${bareCode}`,
    `foolish.cards/${bareCode}?utm_source=imessage`,
  ]) {
    assert.equal(urlToGame(pasted), enc.x,
      `a pasted link named a different game (np=${np}, seed=${rng.seed}): ${pasted}`);
  }
  const xB64 = bytesToBigint(base64ToBytes(enc.base64));
  assert.equal(xUrl, enc.x, `serialization round-trip mismatch (url) (np=${np}, seed=${rng.seed})`);
  assert.equal(xB64, enc.x, `serialization round-trip mismatch (base64) (np=${np}, seed=${rng.seed})`);

  // The decode is the game the engine actually played - compared on the INFO
  // actions, in order and by seat: what the replay says each step played
  // (replay_steps.c) against the moves live play pushed to the table's watchers.
  const steps = replayPlaySteps(enc.bytes, null, null);
  const decodedMoves = steps.filter((s) => INFO_STEP[s.kind]).map((s) => `${INFO_STEP[s.kind]}@${s.seat}`);
  assert.equal(diffStreams(pushedMoves(game), decodedMoves), null,
    `info-action stream mismatch (np=${np}, ${where}, seed=${rng.seed})`);

  // fool / elimination order / trump / seats must match too
  const summary = replaySummary(enc.bytes);
  assert.ok(summary, `the code has a summary (np=${np}, seed=${rng.seed})`);
  assert.equal(summary!.numPlayers, np, `seat count mismatch (np=${np}, seed=${rng.seed})`);
  assert.equal(summary!.fool, game.fool, `fool mismatch (np=${np}, seed=${rng.seed})`);
  assert.deepEqual(summary!.elimination, game.spectatorView.elimination, `elimination mismatch (np=${np}, seed=${rng.seed})`);
  assert.equal(summary!.powerSuit, game.spectatorView.powerSuit, `trump mismatch (np=${np}, seed=${rng.seed})`);
  assert.equal(summary!.moves, decodedMoves.length, 'the summary counts the moves the extras time');

  // The replay screen must be able to replay this code, and land on the game
  // that was played.
  const frames = buildReplayFrames(enc.bytes, 'g', null, { fool: summary!.fool });
  assert.ok(frames.length > 1, 'the code replays to steps');
  const lastFrame = frames[frames.length - 1];
  assert.equal(lastFrame.game.discardPileLength, game.spectatorView.discardPileLength,
    `discard pile mismatch (np=${np}, seed=${rng.seed})`);
  lastFrame.game.seats.forEach((p, s) => {
    // The engine's own out-flags: at game end everyone but the fool is out,
    // including the seats emptied by a refill, which log nothing.
    assert.equal(p.status === V.PLAYER_STATUS_OUT, s !== summary!.fool, `out-flag mismatch at seat ${s}`);
    assert.equal(p.handCount, game.seatViews[s].myHand.length, `seat ${s} ends on its real hand size`);
  });

  // extras (names + per-move timing) round-trip: the finalize path's extras,
  // from the table's roster and the session log's commit clock.
  const table = fixtureTable();
  assert.equal(table.load(game.state, game.roster), 0);
  const blob = table.replayExtras(game.log);
  assert.ok(typeof blob !== 'number', `extras refused (${blob})`);
  const extras = kernelB32Encode(blob as Uint8Array);
  const full = joinReplayCode(enc.base32, extras);
  const { moves: m2, extras: x2 } = splitReplayCode(full);
  assert.equal(m2, enc.base32, 'extras container split mismatch (moves)');
  assert.equal(x2, extras, 'extras container split mismatch (extras)');
  assert.equal(urlToGame('WWW.FOOLISH.CARDS/' + full), enc.x, 'urlToGame must ignore the extras section');

  const back = decodeExtras(extras, np, summary!.moves);
  back.names!.forEach((nm, i) => {
    const orig = NAMES[i];
    assert.ok(nm === orig || orig.startsWith(nm), `name mismatch: ${nm} vs ${orig}`);
  });
  assert.equal(back.moveGaps!.length, summary!.moves, 'extras: a gap per move the code decodes');
  // The deal is committed at the helper's clock origin, each later cycle a second on.
  assert.ok(Math.abs(back.startTime! - 1_700_000_000) <= 1, `start time mismatch: ${back.startTime}`);
  back.moveGaps!.forEach((g, i) => {
    const want = Math.round(g);
    assert.ok(Math.abs(g - want) <= Math.max(0.08, want * 0.08), `gap ${i}: got ${g}, want whole seconds`);
  });

  // The bout leader must be the seat that actually opens each bout. Nothing
  // derives it - firstAttacker is on the board the engine committed - so what
  // is left to check is that the board agrees with what happened.
  for (let s = 1; s < frames.length; s++) {
    if (
      frames[s].kind === REPLAY_ATOM_ATTACK &&
      frames[s - 1].game.battles.length === 0 &&
      frames[s].seat !== frames[s - 1].game.firstAttacker
    ) {
      throw new Error(
        `firstAttacker drift at step ${s}: P${frames[s].seat} opened, board says P${frames[s - 1].game.firstAttacker}`,
      );
    }
  }
  return true;
}

// Owns the replay validation scenarios; the fast runner
// (e2e/validation/replay_validation.test.ts) imports `registerReplayValidation`.
export function registerReplayValidation(): void {
  // The tutorial's own frozen code used to be guarded here. It now has a suite
  // of its own - e2e/tutorial_game.test.ts, which replays it rather than just
  // decoding it - registered into this same fast runner by
  // e2e/validation/tutorial_validation.test.ts.

  // The bridge turns the kernel's refusal of a retired format into its
  // sentence, naming the version. What the kernel refuses is c/tests/tests.c's
  // (test_replay_refuses_every_retired_version walks the whole alphabet, and
  // test_replay_answers_or_refuses_any_bytes feeds every reader junk); the
  // over-long refusal is held apart from a malformed input in
  // test_table_replay_code_and_extras, and the extras' time curve in
  // test_extras_time_curve_is_scale_free and test_extras_corpus_round_trips.
  test('a retired format is refused through the bridge by its number', () => {
    // The version is the first symbol, coded uniform over 16, so the integer 7
    // IS a version-7 header.
    assert.throws(
      () => replayPlaySteps(new Uint8Array([7]), null, null),
      /unsupported replay format version 7/,
    );
    assert.equal(replaySummary(new Uint8Array([7])), null, 'a retired code has no summary');
  });

  // A few short engine games round-trip byte-exact (engine<->replay drift guard).
  test('replay codec round-trips short engine games byte-exact (2..4 players)', () => {
    let played = 0;
    for (let np = 2; np <= 4; np++) {
      for (let g = 0; g < 2; g++) {
        const brain = g % 2 === 0 ? 'random' : 'handwritten';
        const game = playRandomGame(np, brain);
        // A game the kernel refuses as too long has no code to compare; it is
        // not a byte-exactness failure. Rare at 2..4 seats, but the outcome is
        // legal at every seat count, so do not count it as a game played.
        if (roundTripGame(game, np, `np=${np} game=${g} brain=${brain}`)) played++;
      }
    }
    assert.ok(played > 0, 'at least one short game completed');
  });
}

if (!process.env.VALIDATION_ONLY) registerReplayValidation();

// v6 refuses to encode a session whose log outran the kernel's MAX_LOGS
// (c/src/game.h) - the stream is truncated at that point, so there is no honest
// code to cut, and production accepts that a game that long gets no share code.
// The refusal has its own code, REPLAY_ETOOLONG (ebcc7a1), so it can be told
// apart from a genuinely malformed input by the code alone: roundTripGame
// returns false for it and throws for everything else. A too-long game is
// skipped and counted here; anything that raises EINPUT still fails the suite.
//
// This is why the suite does NOT pre-screen on a log count of its own. MAX_LOGS
// is a BUILD PARAMETER (c/Makefile overrides it per artifact), so a copy of it
// here would be a second source of truth that drifts silently. Ask the
// encoder; it is the one that knows.

if (!process.env.VALIDATION_ONLY) test(`replay codec round-trips engine-played games (${GAMES_PER_PC}/player-count, 2..8 players)`, () => {
  let totalGames = 0;
  let refused = 0;
  for (let np = 2; np <= 8; np++) {
    for (let g = 0; g < GAMES_PER_PC; g++) {
      // Alternate fast brains for distribution diversity: random explores the
      // legal-move space (correctness), handwritten produces realistic game
      // shapes. (The Monte-Carlo bots are omitted - their searches are far too
      // slow for a codec test, and add no coverage the moves don't already get.)
      const brain = g % 2 === 0 ? 'random' : 'handwritten';
      const game = playRandomGame(np, brain);
      if (roundTripGame(game, np, `np=${np} game=${g} brain=${brain}`)) totalGames++;
      else refused++;
    }
  }
  // eslint-disable-next-line no-console
  console.log(`[replay codec] ${totalGames} games round-tripped, `
    + `${refused} refused as too long (no code by design), seed=${rng.seed}`);
  assert.ok(totalGames > 0, `no games completed (seed=${rng.seed})`);
  // A ceiling that swallowed the suite would otherwise pass silently.
  assert.ok(refused <= totalGames * 0.05,
    `${refused} of ${totalGames + refused} games overran the log buffer: `
    + `the harness is skipping its own coverage (seed=${rng.seed})`);
});
