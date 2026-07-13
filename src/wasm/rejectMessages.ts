// Short English strings for the packed action path's reject codes — a
// client-side mirror of ENGINE_REJECT_* in cnitro/src/game.h (the kernel that
// produced the code). These are diagnostic only: the client console.errors
// them and reverts optimistic state; nothing renders them.

export const REJECT_MESSAGES: Record<number, string> = {
    1: 'NOT_PLAYING: game is not in progress',
    2: 'EMPTY: no cards in the move',
    3: 'IS_DEFENDER: the defender cannot attack',
    4: 'NOT_DEFENDER: only the defender can do this',
    5: 'NOT_IN_HAND: card not in hand',
    6: 'DUPLICATES: duplicate cards in the move',
    7: 'NOT_SAME_VALUE: cards must share one value',
    8: 'NOT_FIRST_ATTACKER: waiting for the first attacker',
    9: 'VALUE_NOT_ON_TABLE: attack value not on the table',
    10: 'DEFENDER_CAPACITY: defender cannot hold that many attacks',
    11: 'NO_UNCOVERED: nothing uncovered to act on',
    12: 'ATTACK_NOT_ON_TABLE: target attack is not on the table',
    13: 'CANNOT_COVER: card cannot beat that attack',
    14: 'NO_TABLE_CARDS: the table is empty',
    15: 'COVER_PRESENT: cannot pass over a covered battle',
    16: 'PASS_VALUES: pass cards must match the table value',
    17: 'PASS_CAPACITY: next defender cannot hold the passed attacks',
    18: 'NOT_IN_STATUS: player is not in play',
    19: 'ALREADY_GOOD: already said good',
    20: 'FIRST_MUST_ATTACK: the first attacker must open',
    21: 'PASS_OVERFLOW: too many cards to pass',
};

// Server-edge reject codes live above the kernel's ENGINE_REJECT_* range (see
// wire/awire.ts REJECT_STALE_ROUND): the move was kernel-legal but refused by
// an edge policy — here, a round closed before it landed.
const REJECT_STALE_ROUND = 100;

// -1 is the guards/rules kernels' "malformed wire" verdict (not an
// ENGINE_REJECT_* code); anything else unknown falls through generically.
export function rejectMessage(code: number): string {
    if (code === -1) return 'MALFORMED: unreadable action wire';
    if (code === REJECT_STALE_ROUND) return 'STALE_ROUND: a round closed before this move landed';
    return REJECT_MESSAGES[code] ?? `move rejected (code ${code})`;
}
