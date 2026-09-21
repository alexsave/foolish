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

// A COVER RUN IS ONE MOVE. The replay wire groups an attack and a pass - both
// have a continuation loop - but a cover is coded one pair at a time
// (c/src/replay.c atom_cover), so a defender who takes three attacks in one
// move comes back as three steps. Measured on a shared 65-step game: attacks
// arrive carrying 1, 2 or 3 cards, and all 21 covers arrive carrying exactly 1.
//
// The kernel already reconstructs this rather than storing it. iMessage animates
// a TURN, and finds one by walking back over consecutive steps of the same actor
// (c/ios/ios_api_replay.c: "back over every step that seat played immediately
// before it"). That is the rule below, applied to covers only - the one kind the
// wire is known to split. Grouping any other kind would be inventing a move.
//
// Honest about what it cannot know: two covers in a row genuinely could have
// been one multi-cover or two separate ones, and the wire dropped the bit that
// would say. A seat that takes several attacks with nobody acting in between
// played one move in every case this can actually produce - a bot picks one move
// from a menu that offers the multi-cover, and the board does not hand the turn
// back mid-cover - so grouping is right where it is not provable.
const isCoverBy = (f: ReplayFrame | undefined, seat: number | null): boolean =>
    !!f && f.kind === REPLAY_STEP.COVER && f.seat === seat;

/** The first step of the cover run `j` sits in - `j` itself for anything else. */
function coverRunStart(frames: ReplayFrame[], j: number): number {
    if (frames[j].kind !== REPLAY_STEP.COVER) return j;
    let i = j;
    while (i > 1 && isCoverBy(frames[i - 1], frames[j].seat)) i--;
    return i;
}

/** The last step of the cover run that starts at `i`. */
function coverRunEnd(frames: ReplayFrame[], i: number): number {
    if (frames[i].kind !== REPLAY_STEP.COVER) return i;
    let k = i;
    while (k + 1 < frames.length && isCoverBy(frames[k + 1], frames[i].seat)) k++;
    return k;
}

/** The decision step under the cursor: the nearest decision at or before the
 *  paused step, and for a cover the step its whole run STARTS at, so every step
 *  of one multi-cover names the same decision and the board it was decided on. */
export function findDecisionIndex(frames: ReplayFrame[], stepIdx: number): number | null {
    for (let j = Math.min(stepIdx, frames.length - 1); j >= 1; j--) {
        if (frames[j].seat !== null && ORACLE_DECISION_KINDS.has(frames[j].kind)) {
            return coverRunStart(frames, j);
        }
    }
    return null;
}

/** Canonical key + human label of the move recorded at step `j` - for a cover,
 *  every pair of its run (see coverRunStart). */
function recordedMove(frames: ReplayFrame[], j: number, trump: number): { key: string; label: string } {
    const frame = frames[j];
    const type = KIND_TO_MTYPE[frame.kind] ?? 'wait';
    // THE KERNEL SAYS HOW MANY CARDS THE MOVE NAMED (replay_steps.h, the step
    // index's third byte). A step's cards are not always its move's: a pickup
    // carries the pile it swept, which nobody chose. Slicing to the kernel's
    // count is what stops this side inventing a second answer to a question the
    // kernel already answers for octogen's dump - they disagreed about pickup,
    // and every recorded pickup read as "not considered" because of it.
    const last = type === 'cover' ? coverRunEnd(frames, j) : j;
    const cards: string[] = [];
    const targets: string[] = [];
    for (let k = j; k <= last; k++) {
        const f = frames[k];
        for (const c of f.cards.slice(0, f.named)) cards.push(oracleCardToken(c, trump));
        if (f.named > 0 && f.target) targets.push(oracleCardToken(f.target, trump));
    }
    const key = canonicalMoveKey(type, cards, targets);
    let label: string;
    if (type === 'cover') {
        // The same arrow the candidate rows draw (OracleOverlay moveTitleText):
        // it is a cell on the 15-segment array, where "->" is a dash and a '>'
        // the font has no glyph for, so the header fell out to plain text for
        // two characters in the middle of a readout.
        label = `cover ${cards.map((c, k) => `${c}→${targets[k] ?? '?'}`).join(' ')}`;
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

    const rec = recordedMove(frames, j, pre.powerSuit);
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
