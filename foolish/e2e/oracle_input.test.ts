/* =============================================================================
 * The Oracle's input, as the kernel builds it (c/src/replay_steps.h)
 * =============================================================================
 * The Infinite Oracle deliberates a replay's decision from two byte strings it
 * imports unchanged: the board the acting seat decided on
 * (replay_steps_board_v6, through replayStepMaskedState) and the public log
 * before the move (replay_steps_memory_v6, through replayStepLogs). This holds
 * the two rules about them that no other suite does:
 *
 *   - the memory hides every drawn card. A v6 code carries every draw's real
 *     identity, so a memory that copied it through would hand the Oracle (and
 *     anyone reading its panel) the cards another seat drew;
 *   - the kernel refuses a step that is not a decision it can vouch for, rather
 *     than writing a board or a memory for it.
 *
 * The games are played here on the C Table (helpers/bot_table.ts), so the codes
 * are cut by the kernel under test.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { replayStepCount, replayStepIndex, replayStepLogs, replayStepMaskedState, REPLAY_STEP } from '../sdk/ts/wasm/bots.ts';
import { LOG_DRAW } from '../sdk/ts/gen/game_layout.bots.ts';
import { playBotTable, seedBytes } from './helpers/bot_table.ts';

// The memory's wire (wasm_import_logs): u16 record count, then per record
// u8 type (LOG_*), u8 seat, u8 defender, u8 pair count and the pairs as two
// 1-byte wire cards.
const WIRE_HIDDEN = 0xfe;

test('an Oracle memory hides every drawn card', () => {
    let memories = 0, draws = 0, leaked = 0;
    for (let np = 2; np <= 4; np++) {
        for (let s = 0; s < 3; s++) {
            const { code } = playBotTable(Array(np).fill('handwritten'), seedBytes(np, 300 + s));
            for (let step = 1; step < replayStepCount(code); step++) {
                const wire = replayStepLogs(code, step);
                if (!wire) continue;
                memories++;
                let at = 0;
                const n = wire[at] + 256 * wire[at + 1];
                at += 2;
                for (let i = 0; i < n; i++) {
                    const type = wire[at], pairs = wire[at + 3];
                    at += 4;
                    for (let j = 0; j < pairs; j++, at += 2) {
                        if (type !== LOG_DRAW) continue;
                        draws++;
                        if (wire[at] !== WIRE_HIDDEN) leaked++;
                    }
                }
                assert.equal(at, wire.length, `step ${step}: the memory reads to its end`);
            }
        }
    }
    assert.ok(memories > 100 && draws > 100, `exercised ${memories} memories holding ${draws} draws`);
    assert.equal(leaked, 0, `${leaked} drawn-card identities reached an Oracle memory`);
});

test('the kernel refuses what is not a decision it can vouch for', () => {
    const { code } = playBotTable(['handwritten', 'handwritten', 'handwritten'], seedBytes(3, 41));
    const steps = replayStepCount(code);
    assert.equal(replayStepMaskedState(code, 0, 0), null, 'the deal was decided by nobody');
    assert.equal(replayStepMaskedState(code, steps, 0), null, 'no step past the last');
    assert.equal(replayStepMaskedState(code, 1, 3), null, 'a viewer that is not a seat');
    assert.ok(replayStepMaskedState(code, 1, -1), 'a spectator is a viewer');
    assert.equal(replayStepLogs(code, 0), null, 'the deal has no record');
    assert.equal(replayStepLogs(code, steps), null, 'no step past the last');
    const round = replayStepIndex(code).findIndex((info, i) => i > 0 && info.kind === REPLAY_STEP.ROUND_END);
    assert.ok(round > 0, 'the game has a round end');
    assert.equal(replayStepLogs(code, round), null, 'a round end has no record of its own');
    assert.ok(replayStepMaskedState(code, round, -1), 'but it has a board');
});
