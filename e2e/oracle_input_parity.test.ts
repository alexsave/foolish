/* =============================================================================
 * The Oracle's input, built in C, is the input the TS path built
 * (docs/C_GAME_SHAPE_MIGRATION.md Phase 7)
 * =============================================================================
 * The Infinite Oracle deliberates a replay's decision from two byte strings it
 * imports: the board the acting seat decided on (wasm_import_state, masked) and
 * the public log before the move (wasm_import_logs). They used to be assembled in
 * TS - a Game-shaped object out of the replay's frames, marshalled by
 * engine.ts __marshalGame, and a log wire encoded from the decoded log stream
 * (src/oracle/logsWire.ts). The kernel now writes both
 * (c/src/replay_steps.h replay_steps_board_v6, replay_steps_memory_v6), and this
 * holds them to what the TS path wrote, for every decision of the played games
 * the Oracle suites use (e2e/oracle_replay.test.ts, e2e/oracle_mode_b.test.ts).
 *
 * THE LOG is byte-identical.
 *
 * THE BOARD is identical except where the kernel's masking rule and the TS
 * placeholder differ, and those bytes are named rather than hidden:
 *   - a card the acting seat cannot see (the stock, the other hands) is the
 *     kernel's hidden byte (view.h state_put), where the TS path wrote a five of
 *     spades as a placeholder card;
 *   - with no trump face up, the flipped byte is the hidden byte (state_put's
 *     canonical no-flip byte), where the TS path wrote 0 (an import ignores it);
 *   - the acting seat's turn flag and the good timestamp flag are the replayed
 *     game's own, where the TS path wrote false (no strategy reads either).
 *
 * fixtures/oracle_input/parity.json was recorded from the TS path before it was
 * deleted (commit 86c0c894's version of this file): per decision, the step, the
 * acting seat, and the sha-256 of the TS board with exactly those rules applied
 * (the turn and timestamp flags read off the acting seat's own replay frame, an
 * evwire read, not this entry) and of the TS log. At that commit the C bytes
 * equalled the TS bytes decision by decision (234 decisions, 234 memories), and
 * oracle.wasm's deliberation dump was identical on both, memory on and off, at a
 * fixed seed (468 deliberations).
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';

import { replayStepMaskedState, replayStepLogs } from '../sdk/ts/wasm/bots.ts';
import { buildReplayFrames } from '../src/replay/frames.ts';
import { findDecisionIndex } from '../src/oracle/replayOracleInput.ts';
import { playSeededV6 } from './helpers/seeded_game.ts';

const GOLDEN = 'e2e/fixtures/oracle_input/parity.json';

// The Oracle suites' games: e2e/oracle_replay.test.ts's three and Mode B's one.
const SHAPES: [string, number, number][] = [['3p', 3, 41], ['4p', 4, 42], ['8p', 8, 43], ['2p', 2, 7]];

const sha = (b: Uint8Array | null) => (b ? createHash('sha256').update(b).digest('hex') : null);

/** One decision: its step, the acting seat, and the sha-256 of each input (null: no memory). */
type Row = [step: number, seat: number, state: string, logs: string | null];
type Golden = Record<string, Row[]>;

interface Played {
    code: Uint8Array;
    frames: ReturnType<typeof buildReplayFrames>;
}
const played = new Map<string, Played>();

async function game(label: string, np: number, seed: number): Promise<Played> {
    const hit = played.get(label);
    if (hit) return hit;
    const g = await playSeededV6(np, seed);
    assert.ok(g, `${label}: the seeded game finished`);
    const p = { code: g!.code, frames: buildReplayFrames(g!.code, 'g', null) };
    played.set(label, p);
    return p;
}

const golden = JSON.parse(readFileSync(GOLDEN, 'utf8')) as Golden;

test('every decision: the C board and memory are the TS path\'s bytes', async () => {
    let boards = 0, memories = 0;
    for (const [label, np, seed] of SHAPES) {
        const p = await game(label, np, seed);
        // Every step's decision, as the replay screen finds it under the cursor.
        const decisions = [...new Set(p.frames.map((_, i) => findDecisionIndex(p.frames, i)).filter((j): j is number => j != null))];
        const want = golden[label];
        assert.deepEqual(decisions, want.map(([j]) => j), `${label}: the decisions the TS path saw`);
        for (const [j, seat, state, logs] of want) {
            assert.equal(p.frames[j].seat, seat, `${label} step ${j}: the acting seat`);
            assert.equal(sha(replayStepMaskedState(p.code, j, seat)), state, `${label} step ${j}: the board seat ${seat} decided on`);
            assert.equal(sha(replayStepLogs(p.code, j)), logs, `${label} step ${j}: the log before the move`);
            boards++;
            if (logs) memories++;
        }
    }
    assert.ok(boards > 200 && memories > 150, `covered ${boards} boards, ${memories} memories`);
    console.log(`  [oracle_input_parity] ${boards} decisions, ${memories} memories`);
});

test('the kernel refuses what is not a decision it can vouch for', async () => {
    const p = await game('3p', 3, 41);
    const steps = p.frames.length;
    assert.equal(replayStepMaskedState(p.code, 0, 0), null, 'the deal was decided by nobody');
    assert.equal(replayStepMaskedState(p.code, steps, 0), null, 'no step past the last');
    assert.equal(replayStepMaskedState(p.code, 1, 3), null, 'a viewer that is not a seat');
    assert.ok(replayStepMaskedState(p.code, 1, -1), 'a spectator is a viewer');
    assert.equal(replayStepLogs(p.code, 0), null, 'the deal has no record');
    assert.equal(replayStepLogs(p.code, steps), null, 'no step past the last');
    const round = p.frames.findIndex((f, i) => i > 0 && f.kind === 6);
    assert.ok(round > 0, 'the game has a round end');
    assert.equal(replayStepLogs(p.code, round), null, 'a round end has no record of its own');
    assert.ok(replayStepMaskedState(p.code, round, -1), 'but it has a board');
});
