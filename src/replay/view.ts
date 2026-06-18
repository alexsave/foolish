/* =============================================================================
 * Replay view-state builder (client-only presentation helper — NOT wire
 * format; not generated from supabase/functions/_shared/replay)
 * =============================================================================
 * Folds a DecodedReplay's log stream into one renderable snapshot per event,
 * so the replay screen can step/scrub through the game. Pure data in, pure
 * data out — the screen just renders steps[i].
 * ========================================================================== */

import {
    Card,
    LOG_TYPE,
    LogType,
    PersonalGame,
    PrivatePlayer,
    GAME_STATUS,
    PLAYER_STATUS,
} from '@shared/types.ts';
import { DecodedReplay } from '@shared/replay/core.ts';

export interface ReplaySeatView {
    /** face-down cards (identity unknown to spectators) */
    hidden: number;
    /** publicly known cards in hand (picked up from the table, or the trump) */
    known: Card[];
    /**
     * One entry per hidden card, in draw order, holding the identity the card
     * eventually shows when played — or null if it never surfaces. This is
     * retrodiction for the replay's "reveal hands" toggle: the wire format
     * reveals identities lazily, so a finished game lets us project every
     * future play back onto the hand that held the card. Assignment of plays
     * to draw-slots is FIFO (oldest hidden card first), which is always
     * time-consistent.
     */
    slots: (Card | null)[];
    out: boolean;
    isDefender: boolean;
    good: boolean;
}

export interface ReplayStep {
    kind: LogType | 'end';
    /** acting seat (null for system events) */
    seat: number | null;
    /** cards shown for this event (attack/pass/cover/pickup cards, real draws) */
    cards: Card[];
    /** the attack card being covered (COVER only) */
    target: Card | null;
    /** hidden cards drawn / cards discarded, for message text */
    count: number;
    players: ReplaySeatView[];
    battles: { attack: Card; defense: Card | null }[];
    deckCount: number;
    flipped: Card | null;
    discard: number;
    /** seat leading the current bout (drives the sword marker) */
    firstAttacker: number;
}

const sameCard = (a: Card, b: Card) => a.suit === b.suit && a.value === b.value;

export function buildReplaySteps(d: DecodedReplay): ReplayStep[] {
    const n = d.playerCount;
    const deckSize = n > 4 ? 52 : 36;

    const hidden: number[] = new Array(n).fill(6);
    // shared-reference slots: snapshots keep references to these objects, so
    // an identity assigned when the card is finally played becomes visible in
    // every EARLIER snapshot that held the slot (see materialization below)
    type Slot = { identity: Card | null };
    const slots: Slot[][] = Array.from({ length: n }, () =>
        Array.from({ length: 6 }, () => ({ identity: null })),
    );
    const slotRefs: Slot[][][] = []; // per step, per seat
    const known: Card[][] = Array.from({ length: n }, () => []);
    const out: boolean[] = new Array(n).fill(false);
    const goods = new Set<number>();
    let battles: { attack: Card; defense: Card | null }[] = [];
    let deckCount = deckSize - 6 * n - 1;
    let flipped: Card | null = d.trumpCard;
    let discard = 0;
    let defender = (d.firstAttacker + 1) % n;
    let firstAttacker = d.firstAttacker;
    // a pass moves the defender WITHOUT moving the bout leader; every other
    // defender change re-derives the leader (see executePass vs the
    // cover/pickup/good rotations in the server actions)
    let lastInfoWasPass = false;

    const handCount = (s: number) => hidden[s] + known[s].length;

    // the IN seat from which get_next_player_index reaches `target`
    const previousIn = (target: number): number => {
        for (let k = 1; k <= n; k++) {
            const cand = (target - k + n) % n;
            if (!out[cand]) return cand;
        }
        return target;
    };

    const removeFromHand = (s: number, c: Card) => {
        const i = known[s].findIndex((k) => sameCard(k, c));
        if (i >= 0) {
            known[s].splice(i, 1);
        } else {
            hidden[s]--;
            if (hidden[s] < 0) throw new Error('replay view desync: hand underflow');
            // a fresh reveal: bind the identity to the oldest hidden card
            const slot = slots[s].shift();
            if (!slot) throw new Error('replay view desync: slot underflow');
            slot.identity = c;
        }
    };

    const snapshot = (
        kind: LogType | 'end',
        seat: number | null,
        cards: Card[],
        target: Card | null,
        count: number,
    ): ReplayStep => ({
        kind,
        seat,
        cards: cards.map((c) => ({ ...c })),
        target: target ? { ...target } : null,
        count,
        players: Array.from({ length: n }, (_, s) => ({
            hidden: hidden[s],
            known: known[s].map((c) => ({ ...c })),
            slots: [], // filled in the materialization pass below
            out: out[s],
            isDefender: s === defender && !out[s],
            good: goods.has(s),
        })),
        battles: battles.map((b) => ({
            attack: { ...b.attack },
            defense: b.defense ? { ...b.defense } : null,
        })),
        deckCount,
        flipped: flipped ? { ...flipped } : null,
        discard,
        firstAttacker,
    });

    const steps: ReplayStep[] = [];

    for (const l of d.logs) {
        const primaries = l.card_pairs.map((p) => p.primary);
        switch (l.log_type) {
            case LOG_TYPE.GAME_START:
                break;

            case LOG_TYPE.ATTACK:
            case LOG_TYPE.PASS:
                for (const c of primaries) {
                    battles.push({ attack: c, defense: null });
                    removeFromHand(l.seat!, c);
                }
                goods.clear();
                lastInfoWasPass = l.log_type === LOG_TYPE.PASS;
                break;

            case LOG_TYPE.COVER: {
                const cover = l.card_pairs[0].primary;
                const target = l.card_pairs[0].target!;
                const b = battles.find(
                    (x) => x.defense === null && sameCard(x.attack, target),
                );
                if (!b) throw new Error('replay view desync: cover target');
                b.defense = cover;
                removeFromHand(l.seat!, cover);
                goods.clear();
                lastInfoWasPass = false;
                break;
            }

            case LOG_TYPE.PICKUP:
                known[l.seat!].push(...primaries);
                battles = [];
                goods.clear();
                lastInfoWasPass = false;
                break;

            case LOG_TYPE.GOOD:
                goods.add(l.seat!);
                lastInfoWasPass = false;
                break;

            case LOG_TYPE.DISCARD:
                discard += primaries.length;
                battles = [];
                goods.clear();
                break;

            case LOG_TYPE.DRAW:
                for (const c of primaries) {
                    if (c.suit >= 0) {
                        // the flipped trump is the only identified draw
                        known[l.seat!].push(c);
                        flipped = null;
                    } else {
                        hidden[l.seat!]++;
                        slots[l.seat!].push({ identity: null });
                        deckCount--;
                        if (deckCount < 0)
                            throw new Error('replay view desync: deck underflow');
                    }
                }
                break;

            case LOG_TYPE.PLAYER_OUT:
                out[l.seat!] = true;
                break;

            case LOG_TYPE.DEFENDER_CHANGE:
                defender = l.defender_index!;
                if (!lastInfoWasPass) firstAttacker = previousIn(defender);
                break;
        }

        // refill with an empty stock marks emptied hands out without a log
        // (refillPlayerHandsWithEvents); mirror that for display
        if (deckCount === 0 && flipped === null) {
            for (let s = 0; s < n; s++) if (handCount(s) === 0) out[s] = true;
        }

        const realDraws = primaries.filter((c) => c.suit >= 0);
        slotRefs.push(slots.map((q) => [...q]));
        steps.push(
            snapshot(
                l.log_type,
                l.seat,
                l.log_type === LOG_TYPE.DRAW ? realDraws : primaries,
                l.card_pairs[0]?.target ?? null,
                l.log_type === LOG_TYPE.DRAW
                    ? primaries.length - realDraws.length
                    : primaries.length,
            ),
        );
    }

    // closing step: the fool is whoever is left holding cards
    slotRefs.push(slots.map((q) => [...q]));
    steps.push(snapshot('end', d.fool, [], null, 0));

    // End-of-game deduction: the deck is empty and every player but the fool is
    // out (emptied their hand), so every card except the fool's leftovers turned
    // face-up at some point. The fool's never-played cards are therefore the
    // deck MINUS everything that ever surfaced — knowable by elimination even
    // though they were never played. Fill the still-blank slots with that
    // complement; shared slot refs carry the identity back to earlier snapshots
    // too (exact at the final step; a consistent guess mid-game, same basis as
    // the play-based retrodiction below).
    {
        const ckey = (c: Card) => c.suit * 16 + c.value;
        const seen = new Set<number>();
        for (const l of d.logs) {
            for (const pr of l.card_pairs) {
                if (pr.primary.suit >= 0) seen.add(ckey(pr.primary));
                if (pr.target && pr.target.suit >= 0) seen.add(ckey(pr.target));
            }
        }
        if (d.trumpCard.suit >= 0) seen.add(ckey(d.trumpCard));
        const startValue = deckSize === 52 ? 1 : 5;
        const leftover: Card[] = [];
        for (let suit = 0; suit < 4; suit++)
            for (let v = startValue; v <= 13; v++)
                if (!seen.has(suit * 16 + v)) leftover.push({ suit, value: v });
        let li = 0;
        for (let s = 0; s < n && li < leftover.length; s++)
            for (const slot of slots[s])
                if (slot.identity === null && li < leftover.length)
                    slot.identity = leftover[li++];
    }

    // materialize slot identities: by now every eventually-played hidden card
    // has its identity assigned on the shared slot object
    steps.forEach((step, i) => {
        step.players.forEach((p, s) => {
            p.slots = slotRefs[i][s].map((slot) =>
                slot.identity ? { ...slot.identity } : null,
            );
        });
    });

    // conservation: every card is in a hand, the stock, the discard, or on the
    // table (the table is empty at game end, but check generally)
    const last = steps[steps.length - 1];
    const inHands = last.players.reduce(
        (sum, p) => sum + p.hidden + p.known.length,
        0,
    );
    const onTable = last.battles.reduce(
        (sum, b) => sum + 1 + (b.defense ? 1 : 0),
        0,
    );
    const total =
        inHands + onTable + last.deckCount + (last.flipped ? 1 : 0) + last.discard;
    if (total !== deckSize) {
        throw new Error(
            `replay view desync: ${total} cards accounted for, expected ${deckSize}`,
        );
    }

    return steps;
}

/* ----------------------------------------------------------------------------
 * Synthesize a PersonalGame from a step so the REAL game display components
 * (PlayerRing, TableBattles, DeckAndFlipped, DiscardPile, DefenderShield)
 * render the replay exactly like a live game. Served to them through
 * ReplayServerProvider (ServerContext.tsx).
 * -------------------------------------------------------------------------- */

const REPLAY_VIEWER: PrivatePlayer = {
    player_id: 'replay-viewer',
    name: '',
    status: PLAYER_STATUS.IDLE,
    hand_length: 0,
    is_ai: false,
    hand: [],
    awaiting_attack: false,
    strategy_key: 'human',
};

/** PersonalGame extended with the replay's retroactive hand knowledge,
 *  consumed by the reveal-hands overlay on the replay screen. */
export interface ReplayGameState extends PersonalGame {
    replay_hands: (Card | null)[][];
}

export function stepToGame(
    d: DecodedReplay,
    step: ReplayStep,
    gameId: string,
    names?: string[] | null,
): ReplayGameState {
    const atEnd = step.kind === 'end';
    return {
        id: gameId,
        name: '',
        deck_length: step.deckCount,
        discard_pile_length: step.discard,
        flipped: step.flipped,
        players: step.players.map((p, s) => ({
            player_id: `seat-${s}`,
            name: `${names?.[s] || `P${s + 1}`}${atEnd && d.fool === s ? ' 🃏' : ''}`,
            status: p.out ? PLAYER_STATUS.OUT : PLAYER_STATUS.IN,
            hand_length: p.hidden + p.known.length,
            is_ai: false,
        })),
        status: GAME_STATUS.PLAYING,
        power_suit: d.powerSuit,
        first_attacker: step.firstAttacker,
        defender: step.players.findIndex((p) => p.isDefender),
        table_battles: step.battles,
        elimination_order: [],
        good_timestamp: null,
        good_players: step.players
            .map((p, s) => (p.good ? `seat-${s}` : null))
            .filter((x): x is string => x !== null),
        self: REPLAY_VIEWER,
        replay_hands: step.players.map((p) => [
            ...p.known.map((c) => ({ ...c }) as Card | null),
            ...p.slots.map((c) => (c ? { ...c } : null)),
        ]),
    };
}

/**
 * Absolute unix time (seconds) per step, from the extras blob: GAME_START is
 * the start time, each information-bearing step advances the clock by its
 * gap, and derived steps (draws, discards, rotations, outs) happen "at the
 * same moment" as the action that caused them.
 */
export function stepTimes(
    steps: ReplayStep[],
    startTime: number | null,
    moveGaps: number[] | null,
): (number | null)[] {
    if (startTime === null || !moveGaps) return steps.map(() => null);
    // goods are reconstructed by the decoder (not wire moves), so they carry
    // no recorded gap — they inherit the surrounding clock like other derived
    // steps
    const INFO: (LogType | 'end')[] = [
        LOG_TYPE.ATTACK,
        LOG_TYPE.COVER,
        LOG_TYPE.PASS,
        LOG_TYPE.PICKUP,
    ];
    let t = startTime;
    let g = 0;
    return steps.map((step) => {
        if (INFO.includes(step.kind) && g < moveGaps.length) {
            t += moveGaps[g++];
        }
        return t;
    });
}
