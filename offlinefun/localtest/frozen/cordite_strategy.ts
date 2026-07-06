// Cordite — belief-constrained determinized Monte Carlo. The strongest
// non-cheating bot: sees exactly what a human sees (own hand, table, hand
// counts, deck count, public logs) and runs entirely inside chooseMove.
// Port of cnitro/src/cordite_strategy.c; design + benchmarks in
// cnitro/CORDITE.md. `cordite_max` is the same brain with a larger
// sampled-world budget (measured stronger in C evals; bounded by a
// per-decision wall-clock cap well under the bot-loop budget).

import { Card, Game, GameLog, PLAYER_STATUS } from '../../../supabase/functions/_shared/types.ts';
import { BotStrategy, LegalMove } from '../../../supabase/functions/_shared/bot_interfaces.ts';
import {
    BeliefLog, CorditeParams, CORDITE_PARAMS, CORDITE_MAX_PARAMS,
    MOVE_ATTACK, MOVE_COVER, MOVE_GOOD, MOVE_PASS, MOVE_PICKUP,
    NONE, PublicView, SimMove, corditeChoose, mkCard,
} from './cordite_core.ts';

const toInt = (c: Card | null | undefined): number =>
    (!c || c.suit < 0 || c.value < 0) ? NONE : mkCard(c.suit, c.value);

const toIntList = (cards: Card[] | undefined): number[] =>
    (cards ?? []).map(toInt).filter(c => c !== NONE);

const toSimMove = (m: LegalMove): SimMove => {
    let type: number;
    switch (m.type) {
        case 'attack': type = MOVE_ATTACK; break;
        case 'cover':  type = MOVE_COVER; break;
        case 'pass':   type = MOVE_PASS; break;
        case 'pickup': type = MOVE_PICKUP; break;
        case 'good':   type = MOVE_GOOD; break;
        default:       type = MOVE_GOOD; break;   // 'wait' — never enumerated
    }
    return { type, cards: toIntList(m.cards), attackCards: toIntList(m.attack_cards) };
};

const toBeliefLog = (log: GameLog, playerIdxById: Map<string, number>): BeliefLog => ({
    type: log.log_type as BeliefLog['type'],
    playerIdx: log.player_id !== null ? (playerIdxById.get(log.player_id) ?? -1) : -1,
    pairs: (log.card_pairs ?? []).map(p => ({
        primary: toInt(p.primary),
        target: toInt(p.target ?? null),
    })),
});

const makePublicView = (game: Game, botPlayerId: string): PublicView | null => {
    const myIdx = game.players.findIndex(p => p.player_id === botPlayerId);
    if (myIdx < 0) return null;

    const playerIdxById = new Map<string, number>();
    game.players.forEach((p, i) => playerIdxById.set(p.player_id, i));

    const battlesA: number[] = [];
    const battlesD: number[] = [];
    for (const b of game.table_battles) {
        battlesA.push(toInt(b.attack));
        battlesD.push(b.defense ? toInt(b.defense) : NONE);
    }

    let goodMask = 0;
    for (const pid of game.good_players ?? []) {
        const i = playerIdxById.get(pid);
        if (i !== undefined) goodMask |= 1 << i;
    }

    const elimOrder: number[] = [];
    for (const pid of game.elimination_order ?? []) {
        const i = playerIdxById.get(pid);
        if (i !== undefined) elimOrder.push(i);
    }

    const view: PublicView & { elimOrder: number[] } = {
        numPlayers: game.players.length,
        powerSuit: game.power_suit,
        firstAttacker: game.first_attacker,
        defender: game.defender,
        deckCount: game.deck.length,
        discardLen: game.discard_pile_length,
        flipped: game.flipped ? toInt(game.flipped) : NONE,
        battlesA,
        battlesD,
        myIdx,
        // Own hand only — opponent hands are never read.
        myHand: toIntList(game.players[myIdx].hand),
        handCounts: game.players.map(p => p.hand.length),
        statuses: game.players.map(p => (p.status === PLAYER_STATUS.IN ? 0 : 1)),
        goodMask,
        logs: (game.logs ?? []).map(l => toBeliefLog(l, playerIdxById)),
        elimOrder,
    };
    return view;
};

class CorditeBase implements BotStrategy {
    readonly name: string;
    private params: CorditeParams;

    constructor(name: string, params: CorditeParams) {
        this.name = name;
        this.params = params;
    }

    chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 1) return Promise.resolve(legalMoves[0]);

        try {
            const pv = makePublicView(game, botPlayerId);
            if (!pv) return Promise.resolve(legalMoves[0]);
            const simMoves = legalMoves.map(toSimMove);
            const idx = corditeChoose(pv, simMoves, this.params);
            if (idx >= 0 && idx < legalMoves.length) {
                return Promise.resolve(legalMoves[idx]);
            }
        } catch (error) {
            console.error(`[${this.name}] chooseMove failed, falling back to first legal move:`, error);
        }
        return Promise.resolve(legalMoves[0]);
    }
}

export class CorditeStrategy extends CorditeBase {
    constructor() { super('cordite', CORDITE_PARAMS); }
}

export class CorditeMaxStrategy extends CorditeBase {
    constructor() { super('cordite_max', CORDITE_MAX_PARAMS); }
}
