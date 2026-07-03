// Fulminate — cordite + in-game opponent modeling. Same belief-constrained
// determinized Monte-Carlo brain as cordite, but BEFORE each decision it reads
// the public move log to (1) profile each opponent seat against a set of
// archetypes {handwritten/smart, espresso, random, simple, greedy} and (2)
// SKEW the rollout policy of that seat to its best-fit archetype, so value
// estimates reflect how each opponent actually plays. It does NOT learn across
// games (no cross-game state): every profile is rebuilt from THIS game's logs.
//
// Early game (few/no logs) it falls back to cordite's strong global rollout
// (POL_HANDWRITTEN default), so it behaves like cordite until evidence
// accumulates; it sharpens as the game progresses. Negative inference (an
// opponent declining a beneficial action is evidence about what they lack) is
// captured both in the profiler's pickup/defend feature and, for hand
// sampling, by cordite's existing void/floor belief machinery (buildBelief),
// which fulminate reuses unchanged.
//
// Implementation: a thin wrapper around corditeChoose. The ONLY engine change
// is an optional per-seat rollout-policy override (setSeatPolicy) that is null
// for cordite — when null the engine is bit-for-bit identical to cordite (the
// deterministic fingerprint matches). Deno-compatible: only globalThis/Math/
// Date, no Node imports.

import { Card, Game, GameLog, PLAYER_STATUS } from '../../../supabase/functions/_shared/types.ts';
import { BotStrategy, LegalMove } from '../../../supabase/functions/_shared/bot_interfaces.ts';
import {
    BeliefLog, CorditeParams, CORDITE_PARAMS,
    MOVE_ATTACK, MOVE_COVER, MOVE_GOOD, MOVE_PASS, MOVE_PICKUP,
    NONE, PublicView, SimMove, corditeChoose, mkCard,
    profileSeats, seatWeightsFromProfiles, setSeatWeights,
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
        myHand: toIntList(game.players[myIdx].hand),
        handCounts: game.players.map(p => p.hand.length),
        statuses: game.players.map(p => (p.status === PLAYER_STATUS.IN ? 0 : 1)),
        goodMask,
        logs: (game.logs ?? []).map(l => toBeliefLog(l, playerIdxById)),
        elimOrder,
    };
    return view;
};

export class FulminateStrategy implements BotStrategy {
    readonly name = 'fulminate';
    private params: CorditeParams = CORDITE_PARAMS;

    chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 1) return Promise.resolve(legalMoves[0]);

        try {
            const pv = makePublicView(game, botPlayerId);
            if (!pv) return Promise.resolve(legalMoves[0]);
            const simMoves = legalMoves.map(toSimMove);

            // Profile opponents from this game's public log (cheap: one linear
            // pass), build a per-seat POSTERIOR over the rollout-policy basis, and
            // install it for the duration of this single (synchronous) decision.
            // The MC world loop samples each seat's policy from its posterior per
            // world. If no seat carries meaningful non-strong mass, leave the
            // override OFF so the engine runs cordite's exact fast path.
            const profiles = profileSeats(pv);
            const wts = seatWeightsFromProfiles(pv, profiles);
            setSeatWeights(wts);
            try {
                const idx = corditeChoose(pv, simMoves, this.params);
                if (idx >= 0 && idx < legalMoves.length) {
                    return Promise.resolve(legalMoves[idx]);
                }
            } finally {
                setSeatWeights(null);   // always restore cordite-identical default
            }
        } catch (error) {
            console.error(`[fulminate] chooseMove failed, falling back to first legal move:`, error);
            setSeatWeights(null);
        }
        return Promise.resolve(legalMoves[0]);
    }
}
