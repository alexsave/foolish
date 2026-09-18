/* =============================================================================
 * v6 as the WEB consumes it
 * =============================================================================
 * What a v6 code IS, and that it round-trips every hidden card, is asserted
 * natively by c/tests/replay_v6_test.c - on real engine games, against the
 * kernel's own two producers. This file used to re-run all of that through a TS
 * bridge, and kept ~390 lines of TS choreography (reconstructSeededDeal +
 * collectV6 + marshalInputV6) alive to do it. Both are gone (A9): a second
 * implementation kept byte-identical by a parity test can only ever say "the
 * copy agrees", never "the answer is right".
 *
 * What is left is the part C cannot see - the WEB's consumption of a v6 code:
 * frames.ts must build fully-resolved hands (the Oracle fix): v6 hides nothing,
 * so no slot may be a retrodicted guess.
 *
 * The code comes from the PRODUCTION producer: a C Table played through the
 * kernel's bot cycle and encoded by table_replay_code, the call the finalize path
 * makes (helpers/replay_play.ts), so what is tested here is what actually ships.
 *
 * The other half this file held - that the Oracle's memory of a v6 replay
 * DRAW-masks, so a replay never leaks a drawn card a live game would hide - read
 * the memory's log wire byte by byte in TS. It lives where that wire is written
 * now: e2e/oracle_input.test.ts reads every memory the kernel builds for played
 * games and holds every draw in it hidden.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { replaySummary } from '../sdk/ts/wasm/bots.ts';
import { buildReplayFrames } from '../src/replay/frames.ts';
import { cardKey, playRecorded } from './helpers/replay_play.ts';

if (!process.env.E2E_VERBOSE) {
  console.log = () => {}; console.warn = () => {}; console.error = () => {}; console.info = () => {};
}

const GAMES_PER_PC = Number(process.env.REPLAY_GAMES_PER_PC ?? 12);

const seedFor = (np: number, gi: number) =>
  Uint8Array.from({ length: 32 }, (_, i) => (i * 41 + gi * 97 + np * 11 + 5) & 0xff);

test('a v6 replay knows every hand exactly (no retrodiction - the Oracle fix)', () => {
  // This used to assert that the screen's own fold resolved every hidden slot to
  // an identity. The fold is gone (A5): the hands are the ones the engine really
  // dealt and played, read back per seat from the kernel's frames. Same claim,
  // held against the same truth - the hands the table served each seat when the
  // game it played ended.
  let checked = 0;
  for (let np = 2; np <= 4; np++) {
    for (let gi = 0; gi < GAMES_PER_PC; gi++) {
      const played = playRecorded(Array(np).fill('random'), seedFor(np, gi));
      const summary = replaySummary(played.code);
      assert.ok(summary, 'the code has a summary');
      const frames = buildReplayFrames(played.code, 'g', null, { fool: summary!.fool });

      // Nothing is ever unknown: a v6 replay does not guess, at any step.
      for (const f of frames)
        for (const hand of f.game.replay_hands)
          assert.ok(hand.every((c) => c !== null),
            'every card in every hand has an identity (no retrodicted guess)');

      // Exactness: at the final step, each seat's hand IS its true hand.
      const last = frames[frames.length - 1];
      for (let s = 0; s < np; s++) {
        const got = new Set(last.game.replay_hands[s].map((c) => cardKey(c!)));
        const want = new Set(played.seatViews[s].myHand.map(cardKey));
        assert.equal(got.size, want.size, `seat ${s} final hand size`);
        for (const k of want) assert.ok(got.has(k), `seat ${s} final hand card ${k}`);
      }
      checked++;
    }
  }
  assert.ok(checked > 0, 'exercised v6 replays through frames.ts');
});
