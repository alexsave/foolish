/* =============================================================================
 * Infinite Oracle — the paused decision, as an OracleJob (§5.1, §8.4)
 * =============================================================================
 * Builds the marshal-shaped pre-move state, the recorded-move key, and the
 * memory-ON log wire.
 *
 * The position used to be the replay pipeline's RETRODICTED one — exact at game
 * end, a consistent public guess before it. It is now the position the engine
 * actually replayed, hands and all (src/replay/frames.ts), so the Oracle
 * deliberates over what was really on the table. §5.2's approximation is gone,
 * and `approx` with it.
 *
 * The Oracle still does NOT get to see everyone's cards: it reasons from the
 * acting seat's view, so the other seats stay count-only placeholders. Handing
 * it the exact hands we now hold would not be a better analysis, it would be a
 * different game — one where the bot cheats.
 * ========================================================================== */

import { Card, LOG_TYPE, LogType, GAME_STATUS, PLAYER_STATUS } from '@api/core/types.ts';
import { DecodedReplay, SeatLog } from '@api/common/replay/core.ts';
import { ReplayFrame, REPLAY_STEP } from '../replay/frames';
import { encodeLogsWire } from './logsWire';
import {
    OracleJob, OracleGameState, oracleCardToken, canonicalMoveKey,
} from './types';

// The decisions octogen actually deliberates (derived steps excluded).
const ORACLE_DECISION_KINDS: ReadonlySet<number> = new Set([
    REPLAY_STEP.ATTACK, REPLAY_STEP.COVER, REPLAY_STEP.PASS,
    REPLAY_STEP.PICKUP, REPLAY_STEP.GOOD,
]);

// A step's kind -> the dump's move-type string (og_ex OG_EX_MTYPE), and the log
// type the same move appears as in the decoder's stream.
const KIND_TO_MTYPE: Record<number, string> = {
    [REPLAY_STEP.ATTACK]: 'attack',
    [REPLAY_STEP.COVER]: 'cover',
    [REPLAY_STEP.PASS]: 'pass',
    [REPLAY_STEP.PICKUP]: 'pickup',
    [REPLAY_STEP.GOOD]: 'good',
};

const KIND_TO_LOG: Record<number, LogType> = {
    [REPLAY_STEP.ATTACK]: LOG_TYPE.ATTACK,
    [REPLAY_STEP.COVER]: LOG_TYPE.COVER,
    [REPLAY_STEP.PASS]: LOG_TYPE.PASS,
    [REPLAY_STEP.PICKUP]: LOG_TYPE.PICKUP,
};

const MOVE_LOGS: ReadonlySet<LogType> = new Set([
    LOG_TYPE.ATTACK, LOG_TYPE.COVER, LOG_TYPE.PASS, LOG_TYPE.PICKUP,
]);

/** The decision step under the cursor: the nearest decision at or before the
 *  paused step. Returns null when none exists (Oracle button disabled). */
export function findDecisionIndex(frames: ReplayFrame[], stepIdx: number): number | null {
    for (let j = Math.min(stepIdx, frames.length - 1); j >= 1; j--) {
        if (frames[j].seat !== null && ORACLE_DECISION_KINDS.has(frames[j].kind)) return j;
    }
    return null;
}

/* -----------------------------------------------------------------------------
 * Pairing the two streams
 * -----------------------------------------------------------------------------
 * The Oracle's memory is the PUBLIC log history before the move, and that lives
 * in the decoder's log stream — which is not the kernel's step stream. They are
 * two orderings of one game and they do not line up record-for-record: a 3p game
 * that logs 92 records when PLAYED decodes to 79, because v6 keeps only a
 * trailing good and the rest are reconstructed, and draws group differently.
 *
 * What they do agree on is the moves. Every attack/cover/pass/pickup is one step
 * and one log, in the same order, so pairing them in sequence is exact. This
 * pairs them once and CHECKS each pair (same move, same seat) rather than
 * trusting the count: if the two streams ever drift, the Oracle refuses to
 * analyse a position instead of quietly analysing the wrong one.
 * -------------------------------------------------------------------------- */
function moveLogIndices(frames: ReplayFrame[], logs: SeatLog[]): (number | null)[] {
    const out: (number | null)[] = new Array(frames.length).fill(null);
    let li = 0;
    for (let i = 0; i < frames.length; i++) {
        const want = KIND_TO_LOG[frames[i].kind];
        if (!want) continue;
        while (li < logs.length && !MOVE_LOGS.has(logs[li].log_type)) li++;
        if (li >= logs.length) return out;
        const log = logs[li];
        // Same move by the same seat, or the pairing is not a pairing.
        out[i] = (log.log_type === want && log.seat === frames[i].seat) ? li : null;
        li++;
    }
    return out;
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
 * decision exists at/before it. `code` seeds the decisionId (§5.1).
 */
export function buildOracleJob(
    frames: ReplayFrame[],
    decoded: DecodedReplay,
    stepIdx: number,
    memoryOn: boolean,
    code: string,
): OracleJob | null {
    const j = findDecisionIndex(frames, stepIdx);
    if (j == null || j < 1) return null;          // step 0 is the deal; j >= 1
    const move = frames[j];
    const pre = frames[j - 1];                    // the pre-move state
    const seat = move.seat;
    if (seat == null || !pre) return null;

    const trump = decoded.powerSuit;
    const n = pre.game.players.length;

    // The acting seat's real hand — what it was actually holding when it chose.
    const hand = pre.game.replay_hands[seat] ?? [];
    if (hand.some((c) => c === null)) return null; // v6 never leaves a hole; refuse if it somehow does
    const actingHand = hand.map((c) => ({ ...(c as Card) }));

    const rec = recordedMove(move, trump);

    const players: OracleGameState['players'] = pre.game.players.map((p, s) => ({
        player_id: `seat-${s}`,
        status: p.status === PLAYER_STATUS.OUT ? PLAYER_STATUS.OUT : PLAYER_STATUS.IN,
        name: p.name,
        is_ai: false,
        hand_length: p.hand_length,
        awaiting_attack: false,                   // inert (§8.4)
        // The acting seat's real hand; every other seat count-only, so the
        // deliberation stays inside one player's knowledge.
        hand: s === seat ? actingHand
                         : Array.from({ length: p.hand_length }, () => ({ suit: 0, value: 5 })),
    }));

    const gameBlob: OracleGameState = {
        id: `oracle:${code}:${j}`,
        status: GAME_STATUS.PLAYING,
        power_suit: trump,
        first_attacker: pre.game.first_attacker,
        defender: pre.game.defender,
        discard_pile_length: pre.game.discard_pile_length,
        flipped: pre.game.flipped ? { ...pre.game.flipped } : null,
        good_players: [...pre.game.good_players],
        good_timestamp: null,
        deck: Array.from({ length: pre.game.deck_length }, () => ({ suit: 0, value: 5 })),
        table_battles: pre.game.table_battles.map((b) => ({
            attack: { ...b.attack },
            defense: b.defense ? { ...b.defense } : null,
        })),
        elimination_order: [...pre.game.elimination_order],
        deterministic_deck: false,
        players,
    };

    // Memory: the public log history before this move. A move the two streams
    // cannot be paired on gets no memory rather than the wrong memory.
    let logsWire: OracleJob['logsWire'] = new Uint8Array(0);
    if (memoryOn) {
        const li = moveLogIndices(frames, decoded.logs)[j];
        if (li == null) return null;
        logsWire = encodeLogsWire(decoded.logs.slice(0, li));
    }

    return {
        decisionId: `${code}:${j}:${memoryOn ? 1 : 0}`,
        seat,
        memoryOn,
        gameBlob,
        logsWire,
        recordedKey: rec.key,
        recordedLabel: rec.label,
        numPlayers: n,
        deckAlive: pre.game.deck_length > 0 || pre.game.flipped !== null,
        // The position is the engine's own now, never a guess (see the header).
        approx: false,
        eliminations: pre.game.elimination_order.length,
    };
}
