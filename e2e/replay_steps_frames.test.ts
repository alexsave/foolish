/* =============================================================================
 * A5 - a replay is the game replayed, and the web renders it as live play
 * =============================================================================
 * docs/C_CORE_CONSOLIDATION.md §4.6 (F4.2 / A5).
 *
 * The kernel rebuilds the real Game a v6 code describes and replays it through
 * the real engine, serializing the SAME packed evwire frames live play
 * broadcasts. This asserts the two halves of that claim that only the web can
 * check:
 *
 *   1. the frames decode with the client's LIVE reader (the client slot's push
 *      reader, sdk/ts/table/client_table.ts), not a replay-specific one; and
 *   2. what comes out is the game the engine actually played: a C Table played
 *      through the kernel's bot cycle (helpers/replay_play.ts), whose closing
 *      board and live pushes are the truth.
 *
 * If those hold, a replay screen has no projection to keep in step with live
 * play, because it is not rendering a projection. It is rendering live play.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { replayEventFrames, replayStepCount } from '../sdk/ts/wasm/bots.ts';
import { clientTable, type TableView } from '../sdk/ts/table/client_table.ts';
import { pushToSequence } from '../src/state/pushSequence.ts';
import { playRecorded } from './helpers/replay_play.ts';

if (!process.env.E2E_VERBOSE) {
    console.log = () => {};
    console.warn = () => {};
    console.error = () => {};
    console.info = () => {};
}

const SEED = Uint8Array.from(Buffer.from('00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff', 'hex'));

test('a v6 replay decodes as LIVE evwire frames, and is the game that was played', () => {
    for (let np = 2; np <= 4; np++) {
        const played = playRecorded(Array(np).fill('handwritten'), SEED);
        const steps = replayStepCount(played.code);
        assert.ok(steps > 0, `${np}p: the code reports a step count`);

        // Spectator: the viewer every replay share is watched as.
        const frames = replayEventFrames(played.code, -1);
        assert.equal(frames.length, steps, `${np}p: one frame per step`);

        let decodedSteps = 0;
        let totalEvents = 0;
        let last: TableView | null = null;
        for (const frame of frames) {
            // The LIVE reader. Not a replay-specific one - that is the point.
            const read = clientTable().readPush(frame, { as3: false, identity: 'none' });
            assert.ok(read, `${np}p: step ${decodedSteps} decodes with the live reader`);
            if (!read) break;
            const seq = pushToSequence(read);
            decodedSteps++;
            totalEvents += seq.events.length;
            // Every frame carries its step's committed board (the trailer) -
            // which is exactly the per-step board a replay scrubber renders.
            last = seq.game;
        }
        assert.equal(decodedSteps, steps, `${np}p: every frame decoded`);
        assert.ok(totalEvents > 0, `${np}p: the frames carry animation events`);

        // The replay IS the game: the board the last frame carries must be the
        // board the engine really finished on.
        assert.ok(last, `${np}p: the frames carry board state`);
        assert.equal(last!.discardPileLength, played.spectatorView.discardPileLength,
            `${np}p: replay ends on the played discard count`);
        assert.equal(last!.deckCount, 0, `${np}p: a finished game drained its stock`);
        assert.equal(last!.seats.length, np, `${np}p: every seat came back`);
    }
});

test('a replay refuses every retired format by number, and names it', () => {
    // Nine formats came before the one that ships and none of them decodes: the
    // deal order changed under 5..8, and 9 hid the deal, so its "hands" were
    // retrodiction and there was no deck to rebuild. The kernel says which
    // version it is looking at instead of guessing at the game (c/src/replay.h).
    //
    // THE CODE HAS TO REALLY CARRY THAT VERSION. The version is the first
    // symbol, coded uniform over 16, so a decoder reads it as `x % 16` - which
    // makes the single byte v the smallest code whose version IS v. This used to
    // pass [0x05,0,0,0] and call it "a v5 code"; that integer is 0x05000000,
    // whose version field is 0, so the assertion never once saw a 5.
    for (const v of [5, 6, 7, 8, 9]) {
        assert.throws(
            () => replayStepCount(new Uint8Array([v])),
            new RegExp(`unsupported replay format version ${v}\\b`),
            `a version-${v} code must be refused by number`,
        );
    }
});
