// OFFLINE-ONLY. A frozen snapshot of cordite "before the changes" — the
// pre-speedup/pre-budget core (cordite_core_old.ts, copied verbatim from the
// deployed production core on origin/main: worlds [16,28,28], maxMillis 1500).
// Used purely to play the NEW cordite
// head-to-head against the OLD one in TS. Not wired into the production
// strategy registry; registered on demand by the h2h harness.

import { Card, Game, GameLog, PLAYER_STATUS } from '@api/core/types.ts';
import { BotStrategy, LegalMove } from '@api/core/bot_interfaces.ts';
import {
    BeliefLog, CorditeParams, CORDITE_PARAMS,
    MOVE_ATTACK, MOVE_COVER, MOVE_GOOD, MOVE_PASS, MOVE_PICKUP,
    NONE, PublicView, SimMove, corditeChoose, mkCard,
} from './cordite_core_old.ts';

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
        default:       type = MOVE_GOOD; break;
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
        myHand: toIntList(game.players[myIdx].hand),
        handCounts: game.players.map(p => p.hand.length),
        statuses: game.players.map(p => (p.status === PLAYER_STATUS.IN ? 0 : 1)),
        goodMask,
        logs: (game.logs ?? []).map(l => toBeliefLog(l, playerIdxById)),
        elimOrder,
    };
    return view;
};

class CorditeOldBase implements BotStrategy {
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
            if (idx >= 0 && idx < legalMoves.length) return Promise.resolve(legalMoves[idx]);
        } catch (error) {
            console.error(`[${this.name}] chooseMove failed:`, error);
        }
        return Promise.resolve(legalMoves[0]);
    }
}

export class CorditeOldStrategy extends CorditeOldBase {
    constructor() { super('cordite_old', CORDITE_PARAMS); }
}
