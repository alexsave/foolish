/* =============================================================================
 * Infinite Oracle — DecodedReplay + steps + stepIdx -> OracleJob (§5.1, §8.4)
 * Builds the marshal-shaped pre-move state, the recorded-move key, and the
 * memory-ON log wire. The position is the replay pipeline's RETRODICTED
 * position (exact at game end; a consistent public guess mid-game, §5.2).
 * ========================================================================== */

import { Card, LOG_TYPE, LogType, GAME_STATUS, PLAYER_STATUS } from '@shared/types.ts';
import { DecodedReplay, SeatLog } from '@shared/replay/core.ts';
import { minValueFor } from '@shared/constants.ts';
import { ReplayStep } from '../replay/view';
import { encodeLogsWire } from './logsWire';
import {
    OracleJob, OracleGameState, oracleCardToken, canonicalMoveKey,
} from './types';

// seat != null decisions octogen actually deliberates (derived events excluded).
const ORACLE_DECISION_TYPES: ReadonlySet<LogType> = new Set([
    LOG_TYPE.ATTACK, LOG_TYPE.COVER, LOG_TYPE.PASS, LOG_TYPE.PICKUP, LOG_TYPE.GOOD,
]);

// LogType -> the dump's move-type string (og_ex OG_EX_MTYPE).
const LOG_TO_MTYPE: Partial<Record<LogType, string>> = {
    [LOG_TYPE.ATTACK]: 'attack',
    [LOG_TYPE.COVER]: 'cover',
    [LOG_TYPE.PASS]: 'pass',
    [LOG_TYPE.PICKUP]: 'pickup',
    [LOG_TYPE.GOOD]: 'good',
};

/** The decision index j under the cursor: the nearest decision at or before the
 *  paused step. Returns null when none exists (Oracle button disabled). */
export function findDecisionIndex(d: DecodedReplay, stepIdx: number): number | null {
    // The step list ends with a synthetic 'end' entry at logs.length where
    // d.logs[i] is undefined; clamp into the real log range first (§5.1).
    let j = Math.min(stepIdx, d.logs.length - 1);
    for (; j >= 0; j--) {
        const l = d.logs[j];
        if (l && l.seat != null && ORACLE_DECISION_TYPES.has(l.log_type)) return j;
    }
    return null;
}

/** Canonical key + human label of a recorded move (a SeatLog). */
function recordedMove(log: SeatLog, trump: number): { key: string; label: string; type: string } {
    const type = LOG_TO_MTYPE[log.log_type] ?? 'wait';
    const pairs = log.card_pairs ?? [];
    const cards = pairs.map((p) => oracleCardToken(p.primary, trump));
    const targets = pairs
        .map((p) => (p.target ? oracleCardToken(p.target, trump) : null))
        .filter((x): x is string => x !== null);
    const key = canonicalMoveKey(type, cards, targets);
    let label: string;
    if (type === 'cover') {
        label = 'cover ' + pairs.map((p) =>
            `${oracleCardToken(p.primary, trump)}->${p.target ? oracleCardToken(p.target, trump) : '?'}`,
        ).join(', ');
    } else if (type === 'pickup') label = 'pickup';
    else if (type === 'good') label = 'good';
    else label = `${type} ${cards.join(' ')}`.trim();
    return { key, label, type };
}

/** Build the acting seat's hand from the step's public retrodiction, filling any
 *  null slot from the unseen complement (§5.2 belt-and-suspenders; should be
 *  unreachable for a replay that survived the conservation check). */
function actingHand(
    step: ReplayStep, seat: number, numPlayers: number,
): { hand: Card[]; approx: boolean } {
    const p = step.players[seat];
    const hand: Card[] = [...p.known.map((c) => ({ ...c }))];
    const nullSlots = p.slots.filter((s) => s === null).length;
    for (const s of p.slots) if (s) hand.push({ ...s });
    if (nullSlots === 0) return { hand, approx: false };

    // Fill null slots deterministically from the lowest unused deck cards so the
    // acting hand never carries an intra-hand duplicate (the marshal must never
    // emit an invalid card). This is an approximation flagged to the UI.
    const used = new Set(hand.map((c) => c.suit * 16 + c.value));
    const minV = minValueFor(numPlayers);
    let need = nullSlots;
    for (let suit = 0; suit < 4 && need > 0; suit++) {
        for (let v = minV; v <= 13 && need > 0; v++) {
            const key = suit * 16 + v;
            if (used.has(key)) continue;
            used.add(key);
            hand.push({ suit, value: v });
            need--;
        }
    }
    return { hand, approx: true };
}

/**
 * Assemble the analysis job for the decision under the cursor, or null when no
 * decision exists at/before it. `code` seeds the decisionId (§5.1).
 */
export function buildOracleJob(
    d: DecodedReplay,
    steps: ReplayStep[],
    stepIdx: number,
    memoryOn: boolean,
    code: string,
): OracleJob | null {
    const j = findDecisionIndex(d, stepIdx);
    if (j == null || j < 1) return null;             // log 0 is GAME_START; j>=1
    const log = d.logs[j];
    const seat = log.seat!;
    const step = steps[j - 1];                        // pre-move state
    if (!step) return null;
    const trump = d.powerSuit;
    const n = d.playerCount;

    const { hand, approx } = actingHand(step, seat, n);
    const rec = recordedMove(log, trump);

    const players: OracleGameState['players'] = step.players.map((pv, s) => {
        const handLen = pv.hidden + pv.known.length;
        return {
            player_id: `seat-${s}`,
            status: pv.out ? PLAYER_STATUS.OUT : PLAYER_STATUS.IN,
            name: `P${s + 1}`,
            is_ai: false,
            hand_length: handLen,
            awaiting_attack: false,                   // inert (§8.4)
            // acting seat: real retrodicted hand; others: count-only placeholders
            hand: s === seat ? hand : Array.from({ length: handLen }, () => ({ suit: 0, value: 5 })),
        };
    });

    const gameBlob: OracleGameState = {
        id: `oracle:${code}:${j}`,
        status: GAME_STATUS.PLAYING,
        power_suit: trump,
        first_attacker: step.firstAttacker,
        defender: step.players.findIndex((pv) => pv.isDefender),
        discard_pile_length: step.discard,
        flipped: step.flipped ? { ...step.flipped } : null,
        good_players: step.players
            .map((pv, s) => (pv.good ? `seat-${s}` : null))
            .filter((x): x is string => x !== null),
        good_timestamp: null,
        deck: Array.from({ length: step.deckCount }, () => ({ suit: 0, value: 5 })),
        table_battles: step.battles.map((b) => ({
            attack: { ...b.attack },
            defense: b.defense ? { ...b.defense } : null,
        })),
        elimination_order: step.eliminationOrder.map((s) => `seat-${s}`),
        deterministic_deck: false,
        players,
    };

    const logsWire = memoryOn ? encodeLogsWire(d.logs.slice(0, j)) : new Uint8Array(0);

    return {
        decisionId: `${code}:${j}:${memoryOn ? 1 : 0}`,
        seat,
        memoryOn,
        gameBlob,
        logsWire,
        recordedKey: rec.key,
        recordedLabel: rec.label,
        numPlayers: n,
        deckAlive: step.deckCount > 0 || step.flipped !== null,
        approx,
        eliminations: step.eliminationOrder.length,
    };
}
