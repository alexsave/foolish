// Semtex — cordite's successor: the same belief-constrained determinized
// Monte-Carlo brain plus the levers that beat cordite itself AND exploit
// weaker opponents, with no rock-paper-scissors regressions (validated
// against cordite/handwritten/espresso/random fields — see cnitro/SEMTEX.md):
//
//   1. EXACT LEAF ENDGAMES IN ROLLOUTS (3+ players): small 2-player
//      deck-empty endgames inside rollouts are solved exactly instead of
//      finished with handwritten policy play. Against opponents that play
//      endgames exactly (cordite, strong humans) the exact model is the
//      realistic one; heads-up it stays off (the root solver owns that
//      phase, and assuming perfect play vs imperfect opponents costs).
//   2. EXTENDED ROOT SOLVE WINDOW: the exact endgame solver engages at <= 24
//      total cards (cordite: 20) — a window where semtex plays perfectly
//      while cordite still samples. Never worse (proven-win/proven-loss
//      information only), measured identical vs handwritten and better vs
//      cordite/espresso.
//   3. PER-SEAT MC-TELLS: seats that provably play strategically (picked up
//      while holding cover, declined a legal attack) lose the
//      heuristic-family floor/void inference — right against MC bots and
//      thinking humans, while heuristic-family opponents keep full
//      inference pressure.
//   4. OPPONENT PROFILING (fulminate's posterior mixture): weak/random
//      seats are rolled out with matching archetype policies and exploited;
//      the profiler's conservative gate keeps strong seats on the exact
//      cordite-identical default.
//
// Same legitimacy contract as cordite: public info only, no LLM, no reading
// hidden state, everything computed inside one chooseMove call.

import { Card, Game, GameLog, PLAYER_STATUS } from '../types.ts';
import { BotStrategy, LegalMove } from '../bot_interfaces.ts';
import {
    BeliefLog, CorditeParams, CORDITE_PARAMS, CORDITE_MAX_PARAMS,
    MOVE_ATTACK, MOVE_COVER, MOVE_GOOD, MOVE_PASS, MOVE_PICKUP,
    NONE, PublicView, SimMove, corditeChoose, mkCard,
    profileSeats, seatWeightsFromProfiles, setSeatWeights,
    SemtexOpts, setSemtexOpts,
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

// Unlike cordite's adapter, semtex also carries defender_index (the MC-tells
// need the defender's identity at each GOOD event).
const toBeliefLog = (log: GameLog, playerIdxById: Map<string, number>): BeliefLog => ({
    type: log.log_type as BeliefLog['type'],
    playerIdx: log.player_id !== null ? (playerIdxById.get(log.player_id) ?? -1) : -1,
    pairs: (log.card_pairs ?? []).map(p => ({
        primary: toInt(p.primary),
        target: toInt(p.target ?? null),
    })),
    defenderIdx: log.defender_index ?? undefined,
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

// The semtex lever configuration (see file header + cnitro/SEMTEX.md).
const semtexOptsFor = (numPlayers: number): SemtexOpts => ({
    // 12-card exact leaves at 3+ players; small (8-card) leaves heads-up,
    // where bigger exact leaves mis-model imperfect opponents. The TS node
    // budget is far below the C bitboard's 3000: a TS solver node costs ~50x
    // a bitboard node (clone + move enumeration + string TT key), and the
    // fail-memo (leafFail) makes small budgets effective — unresolvable
    // leaves are attempted once per decision, not once per world.
    leafCards: numPlayers >= 3 ? 12 : 8,
    leafBudget: 600,
    solveCards: 24,
    noFloors: false,     // floors stay; MC-tells drop them per proven seat
    voidMod: 4,          // cordite's 3-of-4 void mixture
    adapt: true,
});

class SemtexBase implements BotStrategy {
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

            setSemtexOpts(semtexOptsFor(pv.numPlayers));
            // Fulminate's opponent model: per-seat posterior over the archetype
            // rollout policies, sampled per world. Conservatively gated — with
            // few logs or strong-looking seats it stays on the cordite-identical
            // handwritten default.
            const profiles = profileSeats(pv);
            setSeatWeights(seatWeightsFromProfiles(pv, profiles));
            let idx: number;
            try {
                idx = corditeChoose(pv, simMoves, this.params);
            } finally {
                setSemtexOpts(null);
                setSeatWeights(null);
            }
            if (idx < 0 || idx >= legalMoves.length) idx = 0;
            return Promise.resolve(legalMoves[idx]);
        } catch (_e) {
            setSemtexOpts(null);
            setSeatWeights(null);
            return Promise.resolve(legalMoves[0]);
        }
    }
}

export class SemtexStrategy extends SemtexBase {
    constructor() { super('semtex', CORDITE_PARAMS); }
}

export class SemtexMaxStrategy extends SemtexBase {
    constructor() { super('semtex_max', CORDITE_MAX_PARAMS); }
}
