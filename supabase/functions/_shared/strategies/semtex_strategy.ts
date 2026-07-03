// Semtex — cordite's successor: the same belief-constrained determinized
// Monte-Carlo brain plus the levers that beat cordite itself AND exploit
// weaker opponents, with no rock-paper-scissors regressions (validated
// against cordite/handwritten/espresso/random fields — see cnitro/SEMTEX.md):
//
//   1. EXACT LEAF ENDGAMES IN ROLLOUTS (heads-up): small (8-card)
//      2-player deck-empty endgames inside rollouts are solved exactly
//      instead of finished with handwritten policy play. Against opponents
//      that play endgames exactly (cordite, strong humans) the exact model
//      is the realistic one. At 3+ players leaves stay OFF: loss analysis
//      showed larger leaves there distort mid-game values (see SEMTEX.md).
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
    BeliefLog, CorditeParams, CORDITE_PARAMS,
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

// Semtex world budget: cordite's params with the heads-up budget raised to
// the C-measured knee (192/336/336 — see SEMTEX.md "worlds" finding: heads-up
// vs strong opponents was variance-limited, not saturated). The 2 s
// maxMillis cap still bounds the rare long decision; when wall-clock runs
// out the sampler stops gracefully with the worlds completed so far.
const SEMTEX_PARAMS: CorditeParams = {
    worldsFor: (n) => n <= 2 ? [192, 336, 336] : CORDITE_PARAMS.worldsFor(n),
    maxMillis: 2000,
};
// semtex_max: the full C-measured budget at every player count (hunt-3:
// 6x worlds at 3+ players is worth +5..+8pp win vs heuristic/human-style
// fields at pc3/pc4; see SEMTEX.md). Costs ~2x the base tier's CPU per
// decision at 3-4 players — kept out of the base `semtex` deliberately so
// the default bot's Supabase compute cost is unchanged; flip the params if
// strength matters more than cost. maxMillis still caps latency at 2 s.
const SEMTEX_MAX_PARAMS: CorditeParams = {
    worldsFor: (n) => n <= 2 ? [192, 336, 336]
        : n <= 4 ? [168, 336, 336]
        : n <= 6 ? [240, 480, 336] : [240, 480, 288],
    maxMillis: 2000,
};

// The semtex lever configuration (see file header + cnitro/SEMTEX.md).
const semtexOptsFor = (numPlayers: number): SemtexOpts => ({
    // Small (8-card) exact leaves HEADS-UP ONLY. Loss analysis in C showed
    // larger leaves at 3+ players inject "the endgame will be played
    // perfectly" into mid-game values — individually terrible calls that
    // net zero vs cordite and cost wall-clock; the replicated win is the
    // heads-up leaf. The TS node budget is far below the C bitboard's 3000:
    // a TS solver node costs ~50x a bitboard node, and the fail-memo
    // (leafFail) makes small budgets effective — unresolvable leaves are
    // attempted once per decision, not once per world.
    leafCards: numPlayers >= 3 ? 0 : 8,
    leafBudget: 600,
    solveCards: 24,
    noFloors: false,     // floors stay; MC-tells drop them per proven seat
    voidMod: 4,          // cordite's 3-of-4 void mixture
    adapt: true,
});

class SemtexBase implements BotStrategy {
    readonly name: string;
    private params: CorditeParams;
    private optsFor: (numPlayers: number) => SemtexOpts;

    constructor(name: string, params: CorditeParams,
                optsFor: (numPlayers: number) => SemtexOpts = semtexOptsFor) {
        this.name = name;
        this.params = params;
        this.optsFor = optsFor;
    }

    chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 1) return Promise.resolve(legalMoves[0]);

        try {
            const pv = makePublicView(game, botPlayerId);
            if (!pv) return Promise.resolve(legalMoves[0]);
            const simMoves = legalMoves.map(toSimMove);

            // Test-only ablation switches (offline harness sets these on
            // globalThis; absent in production).
            const G = globalThis as unknown as
                { SEMTEX_NO_PROFILE?: boolean; SEMTEX_NO_ADAPT?: boolean };
            const opts = this.optsFor(pv.numPlayers);
            if (G.SEMTEX_NO_ADAPT) opts.adapt = false;
            setSemtexOpts(opts);
            // Fulminate's opponent model: per-seat posterior over the archetype
            // rollout policies, sampled per world. Conservatively gated — with
            // few logs or strong-looking seats it stays on the cordite-identical
            // handwritten default.
            if (!G.SEMTEX_NO_PROFILE) {
                const profiles = profileSeats(pv);
                setSeatWeights(seatWeightsFromProfiles(pv, profiles));
            }
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
    constructor() { super('semtex', SEMTEX_PARAMS); }
}

export class SemtexMaxStrategy extends SemtexBase {
    constructor() { super('semtex_max', SEMTEX_MAX_PARAMS); }
}

// Octogen — semtex + the extended exact-solve window (see cnitro/OCTOGEN.md):
// the heads-up deck-empty solver engages at <= 28 total cards (semtex: 24),
// win-hunt only beyond 24 (avoidCards gate — taking a proven win is strictly
// safe; the avoidance pass is skipped out there, which also saves its cost).
// C-measured strictly dominant vs semtex: never worse in any paired cell,
// better in the rare deep-endgame deals. TS node budgets are unchanged (they
// are TS-scale already); an aborted solve falls back to the MC gracefully.
const octogenOptsFor = (numPlayers: number): SemtexOpts => ({
    ...semtexOptsFor(numPlayers),
    solveCards: 28,
    avoidCards: 24,
});

export class OctogenStrategy extends SemtexBase {
    constructor() { super('octogen', SEMTEX_PARAMS, octogenOptsFor); }
}

export class OctogenMaxStrategy extends SemtexBase {
    constructor() { super('octogen_max', SEMTEX_MAX_PARAMS, octogenOptsFor); }
}
