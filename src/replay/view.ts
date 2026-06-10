/* =============================================================================
 * Replay view-state builder (client-only presentation helper — NOT wire
 * format; not generated from supabase/functions/_shared/replay)
 * =============================================================================
 * Folds a DecodedReplay's log stream into one renderable snapshot per event,
 * so the replay screen can step/scrub through the game. Pure data in, pure
 * data out — the screen just renders steps[i].
 * ========================================================================== */

import { Card, LOG_TYPE, LogType } from '../common/types';
import { DecodedReplay, SeatLog } from './core';

export interface ReplaySeatView {
    /** face-down cards (identity unknown to spectators) */
    hidden: number;
    /** publicly known cards in hand (picked up from the table, or the trump) */
    known: Card[];
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
}

const sameCard = (a: Card, b: Card) => a.suit === b.suit && a.value === b.value;

export function buildReplaySteps(d: DecodedReplay): ReplayStep[] {
    const n = d.playerCount;
    const deckSize = n > 4 ? 52 : 36;

    const hidden: number[] = new Array(n).fill(6);
    const known: Card[][] = Array.from({ length: n }, () => []);
    const out: boolean[] = new Array(n).fill(false);
    const goods = new Set<number>();
    let battles: { attack: Card; defense: Card | null }[] = [];
    let deckCount = deckSize - 6 * n - 1;
    let flipped: Card | null = d.trumpCard;
    let discard = 0;
    let defender = (d.firstAttacker + 1) % n;

    const handCount = (s: number) => hidden[s] + known[s].length;

    const removeFromHand = (s: number, c: Card) => {
        const i = known[s].findIndex((k) => sameCard(k, c));
        if (i >= 0) {
            known[s].splice(i, 1);
        } else {
            hidden[s]--;
            if (hidden[s] < 0) throw new Error('replay view desync: hand underflow');
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
                break;
            }

            case LOG_TYPE.PICKUP:
                known[l.seat!].push(...primaries);
                battles = [];
                goods.clear();
                break;

            case LOG_TYPE.GOOD:
                goods.add(l.seat!);
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
                break;
        }

        // refill with an empty stock marks emptied hands out without a log
        // (refillPlayerHandsWithEvents); mirror that for display
        if (deckCount === 0 && flipped === null) {
            for (let s = 0; s < n; s++) if (handCount(s) === 0) out[s] = true;
        }

        const realDraws = primaries.filter((c) => c.suit >= 0);
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
    steps.push(snapshot('end', d.fool, [], null, 0));

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
