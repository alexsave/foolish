/* =============================================================================
 * Infinite Oracle - the paused decision, as an OracleJob (§5.1, §8.4)
 * =============================================================================
 * The position and the memory are the kernel's: the board the acting seat
 * decided on, masked as that seat saw it, and the public log before the move,
 * both written by bots.wasm from the replay code itself
 * (c/src/replay_steps.h replay_steps_board_v6, replay_steps_memory_v6) and
 * handed to oracle.wasm unchanged. This file finds the decision under the
 * cursor and names the recorded move; it reads no byte of either.
 *
 * The Oracle does NOT get to see everyone's cards: it reasons from the acting
 * seat's view, so every card that seat could not see is hidden. Handing it the
 * exact hands a replay holds would not be a better analysis, it would be a
 * different game - one where the bot cheats.
 * ========================================================================== */

import { replayStepLogs, replayStepMaskedState } from '@sdk/ts/wasm/bots.ts';
import { ReplayFrame, REPLAY_STEP } from '../replay/frames';
import { OracleJob, oracleCardToken, canonicalMoveKey } from './types';

// The decisions octogen actually deliberates (derived steps excluded).
const ORACLE_DECISION_KINDS: ReadonlySet<number> = new Set([
    REPLAY_STEP.ATTACK, REPLAY_STEP.COVER, REPLAY_STEP.PASS,
    REPLAY_STEP.PICKUP, REPLAY_STEP.GOOD,
]);

// A step's kind -> the dump's move-type string (og_ex OG_EX_MTYPE).
const KIND_TO_MTYPE: Record<number, string> = {
    [REPLAY_STEP.ATTACK]: 'attack',
    [REPLAY_STEP.COVER]: 'cover',
    [REPLAY_STEP.PASS]: 'pass',
    [REPLAY_STEP.PICKUP]: 'pickup',
    [REPLAY_STEP.GOOD]: 'good',
};

/** The decision step under the cursor: the nearest decision at or before the
 *  paused step. Returns null when none exists (Oracle button disabled). */
export function findDecisionIndex(frames: ReplayFrame[], stepIdx: number): number | null {
    for (let j = Math.min(stepIdx, frames.length - 1); j >= 1; j--) {
        if (frames[j].seat !== null && ORACLE_DECISION_KINDS.has(frames[j].kind)) return j;
    }
    return null;
}

/** Canonical key + human label of a recorded move. */
function recordedMove(frame: ReplayFrame, trump: number): { key: string; label: string } {
    const type = KIND_TO_MTYPE[frame.kind] ?? 'wait';
    const cards = frame.cards.map((c) => oracleCardToken(c, trump));
    const targets = frame.target ? [oracleCardToken(frame.target, trump)] : [];
    const key = canonicalMoveKey(type, cards, targets);
    let label: string;
    if (type === 'cover') {
        label = `cover ${cards.join(' ')}->${targets[0] ?? '?'}`;
    } else if (type === 'pickup') label = 'pickup';
    else if (type === 'good') label = 'good';
    else label = `${type} ${cards.join(' ')}`.trim();
    return { key, label };
}

/**
 * Assemble the analysis job for the decision under the cursor, or null when no
 * decision exists at/before it, or the kernel cannot vouch for its memory (a
 * good, or a move whose log record does not pair with it). `code` is the replay
 * code the frames were built from; `gameId` seeds the decisionId (§5.1).
 */
export function buildOracleJob(
    frames: ReplayFrame[],
    code: Uint8Array,
    stepIdx: number,
    memoryOn: boolean,
    gameId: string,
): OracleJob | null {
    const j = findDecisionIndex(frames, stepIdx);
    if (j == null || j < 1) return null;          // step 0 is the deal; j >= 1
    const move = frames[j];
    const pre = frames[j - 1].game;               // the board the move was made on
    const seat = move.seat;
    if (seat == null) return null;

    const state = replayStepMaskedState(code, j, seat);
    if (!state) return null;
    const logsWire = memoryOn ? replayStepLogs(code, j) : new Uint8Array(0);
    if (!logsWire) return null;

    const rec = recordedMove(move, pre.powerSuit);
    return {
        decisionId: `${gameId}:${j}:${memoryOn ? 1 : 0}`,
        seat,
        memoryOn,
        state,
        logsWire,
        powerSuit: pre.powerSuit,
        recordedKey: rec.key,
        recordedLabel: rec.label,
        numPlayers: pre.seats.length,
        deckAlive: pre.deckCount > 0 || pre.hasFlipped,
        // The position is the engine's own, never a guess (see the header).
        approx: false,
        eliminations: pre.elimination.length,
    };
}
