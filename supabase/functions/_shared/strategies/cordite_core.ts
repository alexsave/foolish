// Cordite core — belief-constrained determinized Monte Carlo (TS port of
// cnitro/src/cordite_strategy.c and the slice of the cnitro C engine it
// needs for internal simulation). See cnitro/CORDITE.md for the design and
// benchmark results.
//
// Legitimacy contract: reads ONLY public information from the real game —
// own hand, table, hand counts, deck count, flipped card, and the public
// move history (game logs, with opponent draws masked as -1/-1 except the
// publicly visible flipped card). Opponent hands are never read; sampled
// worlds are filled from the unseen-card pool under public constraints.
//
// Everything below operates on a compact integer representation:
//   card int = (suit << 4) | value,  suit 0..3, value 1..13 (A=13), -1 none.
// The sim engine mirrors the real action handlers (attack/cover/pass/
// pickup/good + refill rules) closely enough for rollouts; real moves are
// always chosen from the server-provided legal move list, so engine
// discrepancies can only add evaluation noise, never illegal moves.
//
// ---------------------------------------------------------------------------
// TS speed-up & budget (v2.3 TS, this branch) — TS-specific work the C ports
// did not cover. Profiling (coarse CDPROF counters + a names-preserving
// esbuild build under --cpu-prof, plus --trace-gc) found the hot costs:
//   * pc2 deck-empty endgames: the exact alpha-beta SOLVER dominates (>half of
//     mean latency, ~all of the tail); its node count, not allocation, is the
//     cost — V8 scavenges the small solver clones cheaply, so it keeps plain
//     cloneSim. CD_NO_SOLVE isolates its share.
//   * pc4-8 (deck alive): the per-(world x candidate) ROLLOUT trial — its
//     cloneSim() was the #1 allocator.
// Changes (all behavior-PRESERVING; a deterministic full-game outcome
// fingerprint matches the pre-change baseline bit-for-bit, pc2-8 vs
// handwritten + espresso, with and without CD_NO_FASTROLL):
//   1. Pooled rollout trial (cloneSimInto + a reused SimGame) — no per-trial
//      allocation. ~20% fewer GC events / less GC time end-to-end.
//   2. Allocation-free fast rollout chooser (handwrittenRolloutChoose): reused
//      scratch buffers + a single reused returned SimMove instead of per-ply
//      map/filter/slice/sort/Set + object allocations.
//   3. Cheaper TT fingerprint (insertion-sort into a shared buffer + no
//      .slice()/.join()), ~10% off the solver.
// Budget (CORDITE_PARAMS): worlds raised ~3x and maxMillis 1500 -> 2000. ~3x is
// the identical-world saturation knee (6x measured ZERO extra win) so the rest
// of the freed compute goes to WIDER candidate survival in the pruning stages
// (keep ~half then 3, was max(3,n/3) then 2), not more identical worlds.
// Offline result vs handwritten (4-core arena, 80 games/pc, seeded): win%
// pc2 82.5->93.8, pc4 32.5->38.8; no regression vs espresso (pc6/pc8 up).
// Single-core p99 per-decision ~0.7-0.9s, max ~1.1s — well under the 2s cap.
// Ablation knobs (offline only): CD_NO_SOLVE, CD_WORLDMUL, CD_NO_FASTROLL.
// ---------------------------------------------------------------------------

// ---------- cards -------------------------------------------------------

export const NONE = -1;
const ACE = 13;

// Test-only A/B switch (offline harness): set globalThis.CD_NO_FASTROLL=true to
// disable the direct rollout chooser and fall back to enumerate-then-pick. The
// two paths are behavior-identical; this exists only to validate that. Read
// per-call (not cached) so a harness can flip it between runs.
const noFastroll = (): boolean =>
    typeof globalThis !== 'undefined' && (globalThis as { CD_NO_FASTROLL?: boolean }).CD_NO_FASTROLL === true;

// Ablation / tuning knobs (offline harness only; read per-call, not cached):
//   CD_NO_SOLVE=true   — skip the exact endgame solver (measure its share).
//   CD_WORLDMUL=<f>    — scale the per-PC world budget by f (budget sweeps).
const G = (): { CD_NO_SOLVE?: boolean; CD_WORLDMUL?: number } =>
    (typeof globalThis !== 'undefined' ? globalThis : {}) as Record<string, unknown>;
const noSolve = (): boolean => G().CD_NO_SOLVE === true;
const worldMul = (): number => {
    const m = G().CD_WORLDMUL;
    return typeof m === 'number' && m > 0 ? m : 1;
};

// Coarse profiling counters (offline only). Reset via cdProfReset(); read via
// cdProfRead(). Zero-cost in production (a few integer adds on hot calls).
export const CDPROF = {
    cloneSim: 0, sampleWorld: 0, simulate: 0, simTurns: 0,
    applyMove: 0, calcLegal: 0, solveNodes: 0, fastChoose: 0,
};
export const cdProfReset = (): void => {
    CDPROF.cloneSim = 0; CDPROF.sampleWorld = 0; CDPROF.simulate = 0;
    CDPROF.simTurns = 0; CDPROF.applyMove = 0; CDPROF.calcLegal = 0;
    CDPROF.solveNodes = 0; CDPROF.fastChoose = 0;
};
export const cdProfRead = (): typeof CDPROF => ({ ...CDPROF });

const CARDS_PER_PLAYER = 6;

export const mkCard = (suit: number, value: number): number => (suit << 4) | value;
export const cardSuit = (c: number): number => c >> 4;
export const cardValue = (c: number): number => c & 15;

const canCoverInt = (attack: number, defense: number, power: number): boolean => {
    const as = attack >> 4, ds = defense >> 4;
    if (ds !== as) return ds === power && as !== power;
    return (defense & 15) > (attack & 15);
};

const cardScore = (c: number, power: number): number =>
    (c & 15) + ((c >> 4) === power ? 1000 : 0);

// Deck floor: mirrors common_utils.refill_deck (players > 4 → full 52).
export const minValueFor = (numPlayers: number): number => (numPlayers > 4 ? 1 : 5);

// ---------- RNG ---------------------------------------------------------

// Same LCG recurrence the engine family uses; local instance so deliberation
// never touches Math.random ordering of the outer game.
let rngState = 1;
const rngSet = (s: number): void => { rngState = (s >>> 0) || 1; };
const rngNext = (): number => {
    rngState = (Math.imul(rngState, 1664525) + 1013904223) >>> 0;
    return rngState / 4294967296;
};

const xorshift = (s: number): number => {
    s >>>= 0;
    s ^= (s << 13); s >>>= 0;
    s ^= (s >>> 17);
    s ^= (s << 5); s >>>= 0;
    return s === 0 ? 0xB1A570 : s;
};

const mix = (a: number, b: number): number => {
    let h = (Math.imul(a >>> 0, 0x9E3779B1) ^ ((b >>> 0) + 0x7F4A7C15)) >>> 0;
    h ^= h >>> 16; h = Math.imul(h, 0x85EBCA77) >>> 0; h ^= h >>> 13;
    return h === 0 ? 1 : h >>> 0;
};

// ---------- sim game ----------------------------------------------------

const ST_IN = 0, ST_OUT = 1;

export interface SimGame {
    over: boolean;            // game aborted / invalid state
    numPlayers: number;
    powerSuit: number;
    firstAttacker: number;
    defender: number;
    deck: number[];           // draw = random index (mirrors common_utils.draw)
    flipped: number;          // NONE once taken
    battlesA: number[];       // attack cards
    battlesD: number[];       // defense cards (NONE = uncovered)
    discardLen: number;
    pStatus: number[];        // ST_IN / ST_OUT
    hands: number[][];
    elim: number[];           // player indices in out order
    goodMask: number;
    // Minimal log: discard events only (the single log feature any rollout
    // policy reads — espresso's discard memory). Flat list of discarded cards.
    discards: number[];
}

const cloneSim = (g: SimGame): SimGame => (CDPROF.cloneSim++, {
    over: g.over,
    numPlayers: g.numPlayers,
    powerSuit: g.powerSuit,
    firstAttacker: g.firstAttacker,
    defender: g.defender,
    deck: g.deck.slice(),
    flipped: g.flipped,
    battlesA: g.battlesA.slice(),
    battlesD: g.battlesD.slice(),
    discardLen: g.discardLen,
    pStatus: g.pStatus.slice(),
    hands: g.hands.map(h => h.slice()),
    elim: g.elim.slice(),
    goodMask: g.goodMask,
    discards: g.discards.slice(),
});

// ---------- pooled SimGame clones (GC elimination) -----------------------
// The dominant TS cost was GC churn from the per-(world x candidate) rollout
// trial, which did a full cloneSim() — 8 array allocations — every trial. We
// instead reuse a single pooled SimGame: cloneSimInto() copies `world` into it
// (reusing its buffers, no allocation), simulate() mutates it to completion,
// then it returns to the pool. cloneSimInto reproduces cloneSim's deep copy
// exactly (same fields, same array contents), so the rollout is behavior-
// identical — verified by a deterministic full-game outcome fingerprint that
// matches the pre-change baseline bit-for-bit (pc2-8 vs handwritten+espresso).
// (Make/unmake was tried for the solver and lost: snapshot+restore is two
// copies per node vs cloneSim's one, and V8 scavenges those small short-lived
// arrays cheaply, so the solver keeps plain cloneSim.)
const simPool: SimGame[] = [];
let simPoolN = 0;

const copyInto = (dst: number[], src: number[]): void => {
    dst.length = src.length;
    for (let i = 0; i < src.length; i++) dst[i] = src[i];
};

const acquireSim = (n: number): SimGame => {
    if (simPoolN > 0) {
        const s = simPool[--simPoolN];
        while (s.hands.length < n) s.hands.push([]);
        return s;
    }
    return {
        over: false, numPlayers: n, powerSuit: 0, firstAttacker: 0, defender: 0,
        deck: [], flipped: NONE, battlesA: [], battlesD: [], discardLen: 0,
        pStatus: [], hands: Array.from({ length: n }, () => [] as number[]),
        elim: [], goodMask: 0, discards: [],
    };
};
const releaseSim = (g: SimGame): void => { simPool[simPoolN++] = g; };

// Copy src into a pooled dst (reusing dst's buffers). Behaviorally identical to
// cloneSim's deep copy.
const cloneSimInto = (dst: SimGame, src: SimGame): SimGame => {
    dst.over = src.over; dst.numPlayers = src.numPlayers; dst.powerSuit = src.powerSuit;
    dst.firstAttacker = src.firstAttacker; dst.defender = src.defender;
    dst.flipped = src.flipped; dst.discardLen = src.discardLen; dst.goodMask = src.goodMask;
    copyInto(dst.deck, src.deck);
    copyInto(dst.battlesA, src.battlesA);
    copyInto(dst.battlesD, src.battlesD);
    copyInto(dst.pStatus, src.pStatus);
    copyInto(dst.elim, src.elim);
    copyInto(dst.discards, src.discards);
    while (dst.hands.length < src.numPlayers) dst.hands.push([]);
    for (let i = 0; i < src.numPlayers; i++) copyInto(dst.hands[i], src.hands[i]);
    return dst;
};

const inCount = (g: SimGame): number => {
    let n = 0;
    for (let i = 0; i < g.numPlayers; i++) if (g.pStatus[i] === ST_IN) n++;
    return n;
};

const gameDone = (g: SimGame): number => {
    let inC = 0, outC = 0, lastIn = -1;
    for (let i = 0; i < g.numPlayers; i++) {
        if (g.pStatus[i] === ST_IN) { inC++; lastIn = i; }
        else outC++;
    }
    return (inC === 1 && outC === g.numPlayers - 1) ? lastIn : -1;
};

const nextPlayer = (g: SimGame, current: number): number => {
    let next = (current + 1) % g.numPlayers;
    while (g.pStatus[next] === ST_OUT) next = (next + 1) % g.numPlayers;
    return next;
};

const handRemove = (hand: number[], c: number): void => {
    const i = hand.indexOf(c);
    if (i >= 0) hand.splice(i, 1);
};

const noCardsLeft = (g: SimGame): boolean => g.deck.length === 0 && g.flipped === NONE;

const drawCard = (g: SimGame): number => {
    if (g.deck.length === 0) {
        if (g.flipped === NONE) return NONE;
        const c = g.flipped;
        g.flipped = NONE;
        return c;
    }
    let idx = Math.floor(rngNext() * g.deck.length);
    if (idx < 0) idx = 0;
    if (idx >= g.deck.length) idx = g.deck.length - 1;
    return g.deck.splice(idx, 1)[0];
};

const refillHands = (g: SimGame): void => {
    if (noCardsLeft(g)) {
        for (let i = 0; i < g.numPlayers; i++) {
            if (g.hands[i].length === 0 && g.pStatus[i] === ST_IN) {
                g.pStatus[i] = ST_OUT;
                g.elim.push(i);
            }
        }
        return;
    }
    if (g.hands[g.defender].length === 0) {
        const hand = g.hands[g.defender];
        while (hand.length < CARDS_PER_PLAYER) {
            const c = drawCard(g);
            if (c === NONE) break;
            hand.push(c);
        }
    }
    let p = g.firstAttacker;
    const visited = new Set<number>();
    do {
        if (visited.has(p)) break;
        visited.add(p);
        const hand = g.hands[p];
        while (hand.length < CARDS_PER_PLAYER) {
            const c = drawCard(g);
            if (c === NONE) break;
            hand.push(c);
        }
        if (hand.length === 0 && g.pStatus[p] === ST_IN) {
            g.pStatus[p] = ST_OUT;
            g.elim.push(p);
        }
        p = nextPlayer(g, p);
    } while (p !== g.firstAttacker);
};

const countUncovered = (g: SimGame): number => {
    let n = 0;
    for (let i = 0; i < g.battlesA.length; i++) if (g.battlesD[i] === NONE) n++;
    return n;
};

const tableHasValue = (g: SimGame, v: number): boolean => {
    for (let i = 0; i < g.battlesA.length; i++) {
        if ((g.battlesA[i] & 15) === v) return true;
        if (g.battlesD[i] !== NONE && (g.battlesD[i] & 15) === v) return true;
    }
    return false;
};

const discardTable = (g: SimGame): void => {
    for (let i = 0; i < g.battlesA.length; i++) {
        g.discards.push(g.battlesA[i]);
        if (g.battlesD[i] !== NONE) g.discards.push(g.battlesD[i]);
        g.discardLen += g.battlesD[i] !== NONE ? 2 : 1;
    }
    g.battlesA.length = 0;
    g.battlesD.length = 0;
};

// ---------- sim actions (mirror cnitro game.c handlers) ------------------

const simAttack = (g: SimGame, pIdx: number, cards: number[]): boolean => {
    if (g.over || cards.length === 0 || pIdx === g.defender) return false;
    const hand = g.hands[pIdx];
    for (let i = 0; i < cards.length; i++) {
        if (hand.indexOf(cards[i]) < 0) return false;
        for (let j = i + 1; j < cards.length; j++) if (cards[i] === cards[j]) return false;
    }
    const firstAttack = g.battlesA.length === 0;
    if (firstAttack) {
        for (let i = 1; i < cards.length; i++) {
            if ((cards[i] & 15) !== (cards[0] & 15)) return false;
        }
        if (pIdx !== g.firstAttacker) return false;
    } else {
        for (const c of cards) if (!tableHasValue(g, c & 15)) return false;
    }
    if (g.hands[g.defender].length < countUncovered(g) + cards.length) return false;

    for (const c of cards) {
        handRemove(hand, c);
        g.battlesA.push(c);
        g.battlesD.push(NONE);
    }
    g.goodMask = 0;
    if (hand.length === 0) {
        g.pStatus[pIdx] = ST_OUT;
        g.elim.push(pIdx);
    }
    return true;
};

const simCover = (g: SimGame, pIdx: number, covers: number[], attacks: number[]): boolean => {
    if (g.over || covers.length === 0 || pIdx !== g.defender) return false;
    if (countUncovered(g) === 0) return false;
    const hand = g.hands[pIdx];
    for (let i = 0; i < covers.length; i++) {
        if (hand.indexOf(covers[i]) < 0) return false;
        for (let j = i + 1; j < covers.length; j++) if (covers[i] === covers[j]) return false;
    }
    for (let i = 0; i < covers.length; i++) {
        let found = false;
        for (let j = 0; j < g.battlesA.length; j++) {
            if (g.battlesD[j] === NONE && (g.battlesA[j] & 15) === (attacks[i] & 15)) {
                found = true; break;
            }
        }
        if (!found) return false;
        if (!canCoverInt(attacks[i], covers[i], g.powerSuit)) return false;
    }
    for (let i = 0; i < covers.length; i++) {
        let idx = -1;
        for (let j = 0; j < g.battlesA.length; j++) {
            if (g.battlesD[j] === NONE && g.battlesA[j] === attacks[i]) { idx = j; break; }
        }
        if (idx < 0) return false;
        g.battlesD[idx] = covers[i];
        handRemove(hand, covers[i]);
    }

    if (hand.length === 0) {
        // Clean cover: discard, refill (defender first), advance defender.
        discardTable(g);
        refillHands(g);
        g.firstAttacker = g.defender;
        g.goodMask = 0;
        if (g.hands[g.firstAttacker].length === 0) {
            if (g.pStatus[g.firstAttacker] === ST_IN) {
                g.pStatus[g.firstAttacker] = ST_OUT;
                g.elim.push(g.firstAttacker);
            }
            if (inCount(g) === 0) { g.over = true; return true; }
            g.firstAttacker = nextPlayer(g, g.firstAttacker);
        }
        if (inCount(g) <= 1) return true;
        g.defender = nextPlayer(g, g.firstAttacker);
        return true;
    }

    g.goodMask = 0;
    return true;
};

const simPass = (g: SimGame, pIdx: number, cards: number[]): boolean => {
    if (g.over || cards.length === 0 || pIdx !== g.defender) return false;
    if (g.battlesA.length === 0) return false;
    for (let i = 0; i < g.battlesD.length; i++) if (g.battlesD[i] !== NONE) return false;
    const v = cards[0] & 15;
    for (let i = 1; i < cards.length; i++) if ((cards[i] & 15) !== v) return false;
    for (let i = 0; i < g.battlesA.length; i++) if ((g.battlesA[i] & 15) !== v) return false;
    const hand = g.hands[pIdx];
    for (let i = 0; i < cards.length; i++) {
        if (hand.indexOf(cards[i]) < 0) return false;
        for (let j = i + 1; j < cards.length; j++) if (cards[i] === cards[j]) return false;
    }
    const next = nextPlayer(g, g.defender);
    if (g.hands[next].length < cards.length + g.battlesA.length) return false;

    for (const c of cards) {
        handRemove(hand, c);
        g.battlesA.push(c);
        g.battlesD.push(NONE);
    }
    g.goodMask = 0;
    if (noCardsLeft(g) && hand.length === 0) {
        g.pStatus[pIdx] = ST_OUT;
        g.elim.push(pIdx);
    }
    g.defender = next;
    if (countUncovered(g) > g.hands[g.defender].length) {
        g.over = true;   // engine invariant violated; abort sim
        return false;
    }
    return true;
};

const simPickup = (g: SimGame, pIdx: number): boolean => {
    if (g.over || pIdx !== g.defender || g.battlesA.length === 0) return false;
    const hand = g.hands[pIdx];
    for (let i = 0; i < g.battlesA.length; i++) {
        hand.push(g.battlesA[i]);
        if (g.battlesD[i] !== NONE) hand.push(g.battlesD[i]);
    }
    g.battlesA.length = 0;
    g.battlesD.length = 0;
    refillHands(g);
    if (inCount(g) <= 1) { g.goodMask = 0; return true; }
    g.firstAttacker = nextPlayer(g, g.defender);
    g.defender = nextPlayer(g, g.firstAttacker);
    g.goodMask = 0;
    return true;
};

const simGood = (g: SimGame, pIdx: number): boolean => {
    if (g.over || g.pStatus[pIdx] !== ST_IN) return false;
    if (pIdx === g.defender) return false;
    if (g.battlesA.length === 0 && pIdx === g.firstAttacker) return false;
    if (g.goodMask & (1 << pIdx)) return false;
    g.goodMask |= (1 << pIdx);

    let nAttackers = 0;
    let allGood = true;
    for (let i = 0; i < g.numPlayers; i++) {
        if (i !== g.defender && g.pStatus[i] === ST_IN) {
            nAttackers++;
            if (!(g.goodMask & (1 << i))) allGood = false;
        }
    }
    if (nAttackers === 0) allGood = false;

    let allCovered = g.battlesA.length > 0;
    for (let i = 0; i < g.battlesD.length; i++) if (g.battlesD[i] === NONE) allCovered = false;

    if (allGood && allCovered) {
        // Round transition: discard, refill, defender becomes attacker.
        discardTable(g);
        refillHands(g);
        if (inCount(g) <= 1) { g.goodMask = 0; return true; }
        g.firstAttacker = g.defender;
        g.defender = nextPlayer(g, g.firstAttacker);
        g.goodMask = 0;
    }
    return true;
};

const shouldAct = (g: SimGame, pIdx: number): boolean => {
    if (g.over || g.pStatus[pIdx] !== ST_IN) return false;
    const firstAttack = g.battlesA.length === 0;
    const isDef = pIdx === g.defender;
    let allCovered = g.battlesA.length > 0;
    for (let i = 0; i < g.battlesD.length; i++) if (g.battlesD[i] === NONE) allCovered = false;
    if (firstAttack) return pIdx === g.firstAttacker;
    if (isDef) return !allCovered;
    return !(g.goodMask & (1 << pIdx));
};

// ---------- sim legal moves ----------------------------------------------

export const MOVE_ATTACK = 0, MOVE_COVER = 1, MOVE_PASS = 2, MOVE_PICKUP = 3, MOVE_GOOD = 4;

export interface SimMove {
    type: number;
    cards: number[];
    attackCards: number[];   // cover only
}

const MAX_SOLVE_MOVES = 96;

const combinations = (arr: number[], k: number, out: number[][]): void => {
    const buf: number[] = new Array(k);
    const rec = (start: number, depth: number): void => {
        if (depth === k) { out.push(buf.slice()); return; }
        for (let i = start; i <= arr.length - (k - depth); i++) {
            buf[depth] = arr[i];
            rec(i + 1, depth + 1);
        }
    };
    rec(0, 0);
};

const calcAttackMoves = (g: SimGame, pIdx: number, out: SimMove[], firstAttack: boolean): void => {
    const hand = g.hands[pIdx];
    const defenderCards = g.hands[g.defender].length;
    const uncovered = firstAttack ? 0 : countUncovered(g);
    if (firstAttack) {
        const seen = new Set<number>();
        for (const c of hand) {
            const v = c & 15;
            if (seen.has(v)) continue;
            seen.add(v);
            const group = hand.filter(h => (h & 15) === v);
            for (let k = 1; k <= group.length; k++) {
                const combos: number[][] = [];
                combinations(group, k, combos);
                for (const combo of combos) {
                    if (defenderCards >= uncovered + combo.length) {
                        out.push({ type: MOVE_ATTACK, cards: combo, attackCards: [] });
                    }
                }
            }
        }
    } else {
        const tv = new Set<number>();
        for (let i = 0; i < g.battlesA.length; i++) {
            tv.add(g.battlesA[i] & 15);
            if (g.battlesD[i] !== NONE) tv.add(g.battlesD[i] & 15);
        }
        const valid = hand.filter(c => tv.has(c & 15));
        if (valid.length === 0) return;
        for (let k = 1; k <= valid.length; k++) {
            const combos: number[][] = [];
            combinations(valid, k, combos);
            for (const combo of combos) {
                if (defenderCards >= uncovered + combo.length) {
                    out.push({ type: MOVE_ATTACK, cards: combo, attackCards: [] });
                }
                if (out.length > MAX_SOLVE_MOVES) return;
            }
        }
    }
};

const calcPassMoves = (g: SimGame, pIdx: number, out: SimMove[]): void => {
    if (g.battlesA.length === 0) return;
    for (let i = 0; i < g.battlesD.length; i++) if (g.battlesD[i] !== NONE) return;
    const v0 = g.battlesA[0] & 15;
    for (let i = 1; i < g.battlesA.length; i++) if ((g.battlesA[i] & 15) !== v0) return;
    const matching = g.hands[pIdx].filter(c => (c & 15) === v0);
    if (matching.length === 0) return;
    const nextCards = g.hands[nextPlayer(g, g.defender)].length;
    for (let k = 1; k <= matching.length; k++) {
        const combos: number[][] = [];
        combinations(matching, k, combos);
        for (const combo of combos) {
            if (nextCards >= combo.length + g.battlesA.length) {
                out.push({ type: MOVE_PASS, cards: combo, attackCards: [] });
            }
        }
    }
};

// Full cover enumeration (solver only; capped).
const calcCoverMoves = (g: SimGame, pIdx: number, out: SimMove[]): void => {
    const hand = g.hands[pIdx];
    const uncIdx: number[] = [];
    for (let i = 0; i < g.battlesA.length; i++) if (g.battlesD[i] === NONE) uncIdx.push(i);
    if (uncIdx.length === 0) return;
    const options = uncIdx.map(i => ({
        attack: g.battlesA[i],
        covers: hand.filter(c => canCoverInt(g.battlesA[i], c, g.powerSuit)),
    }));

    const emit = (picked: { attack: number, covers: number[] }[]): void => {
        const chosen: number[] = new Array(picked.length);
        const used = new Set<number>();
        const rec = (depth: number): void => {
            if (out.length > MAX_SOLVE_MOVES) return;
            if (depth === picked.length) {
                out.push({
                    type: MOVE_COVER,
                    cards: chosen.slice(),
                    attackCards: picked.map(p => p.attack),
                });
                return;
            }
            for (const c of picked[depth].covers) {
                if (used.has(c)) continue;
                used.add(c);
                chosen[depth] = c;
                rec(depth + 1);
                used.delete(c);
            }
        };
        rec(0);
    };

    for (let k = 1; k <= options.length; k++) {
        const idxCombos: number[][] = [];
        combinations(options.map((_, i) => i), k, idxCombos);
        for (const idxs of idxCombos) {
            const picked = idxs.map(i => options[i]);
            if (picked.some(p => p.covers.length === 0)) continue;
            emit(picked);
            if (out.length > MAX_SOLVE_MOVES) return;
        }
    }
};

// Greedy lowest-cost full cover (rollout policy path; matches handwritten).
const calcCoverGreedy = (g: SimGame, pIdx: number, out: SimMove[]): void => {
    const hand = g.hands[pIdx];
    const unc: number[] = [];
    for (let i = 0; i < g.battlesA.length; i++) if (g.battlesD[i] === NONE) unc.push(g.battlesA[i]);
    if (unc.length === 0) return;
    const used = new Set<number>();
    const covers: number[] = [];
    for (const a of unc) {
        let best = NONE, bestScore = Infinity;
        for (const c of hand) {
            if (used.has(c)) continue;
            if (canCoverInt(a, c, g.powerSuit)) {
                const s = cardScore(c, g.powerSuit);
                if (s < bestScore) { bestScore = s; best = c; }
            }
        }
        if (best === NONE) return;
        used.add(best);
        covers.push(best);
    }
    out.push({ type: MOVE_COVER, cards: covers, attackCards: unc });
};

const calcLegal = (g: SimGame, pIdx: number, lite: boolean): SimMove[] => {
    CDPROF.calcLegal++;
    const out: SimMove[] = [];
    if (g.over) return out;
    const isDef = pIdx === g.defender;
    const firstAttack = g.battlesA.length === 0;
    let allCovered = g.battlesA.length > 0;
    for (let i = 0; i < g.battlesD.length; i++) if (g.battlesD[i] === NONE) allCovered = false;

    if (firstAttack && pIdx === g.firstAttacker) {
        calcAttackMoves(g, pIdx, out, true);
    } else if (isDef && g.battlesA.length > 0) {
        if (lite) calcCoverGreedy(g, pIdx, out);
        else calcCoverMoves(g, pIdx, out);
        if (!allCovered) out.push({ type: MOVE_PICKUP, cards: [], attackCards: [] });
        calcPassMoves(g, pIdx, out);
    } else if (!isDef && g.battlesA.length > 0) {
        if (!(g.goodMask & (1 << pIdx))) {
            calcAttackMoves(g, pIdx, out, false);
            out.push({ type: MOVE_GOOD, cards: [], attackCards: [] });
        }
    }
    return out;
};

const applyMove = (g: SimGame, pIdx: number, m: SimMove): boolean => {
    CDPROF.applyMove++;
    switch (m.type) {
        case MOVE_ATTACK: return simAttack(g, pIdx, m.cards);
        case MOVE_COVER:  return simCover(g, pIdx, m.cards, m.attackCards);
        case MOVE_PASS:   return simPass(g, pIdx, m.cards);
        case MOVE_PICKUP: return simPickup(g, pIdx);
        case MOVE_GOOD:   return simGood(g, pIdx);
        default:          return false;
    }
};

// ---------- rollout policy: handwritten (port of handwritten_strategy.c) --

const totalCardCount = (g: SimGame): number => {
    let table = 0;
    for (let i = 0; i < g.battlesA.length; i++) table += 1 + (g.battlesD[i] !== NONE ? 1 : 0);
    let hands = 0;
    for (let i = 0; i < g.numPlayers; i++) hands += g.hands[i].length;
    return g.deck.length + g.discardLen + table + hands + (g.flipped !== NONE ? 1 : 0);
};

const trumpAttackProbability = (g: SimGame): number => {
    if (g.deck.length > 0 || g.flipped !== NONE) return 0.02;
    const total = Math.max(1, totalCardCount(g));
    let ratio = g.discardLen / total;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    let p = 0.65 + 0.35 * ratio;
    if (p < 0.5) p = 0.5;
    if (p > 0.95) p = 0.95;
    return p;
};

const moveAllNonTrump = (m: SimMove, power: number): boolean =>
    m.cards.every(c => (c >> 4) !== power);
const moveHasTrump = (m: SimMove, power: number): boolean =>
    m.cards.some(c => (c >> 4) === power);
const sumScore = (m: SimMove, power: number): number =>
    m.cards.reduce((s, c) => s + cardScore(c, power), 0);

const pickMaxCardsLowestScore = (moves: SimMove[], power: number, idxs: number[]): number => {
    let maxN = -1;
    for (const i of idxs) if (moves[i].cards.length > maxN) maxN = moves[i].cards.length;
    let best = -1, bestScore = Infinity;
    for (const i of idxs) {
        if (moves[i].cards.length !== maxN) continue;
        const s = sumScore(moves[i], power);
        if (s < bestScore) { bestScore = s; best = i; }
    }
    return best;
};

const handwrittenChoose = (g: SimGame, _pIdx: number, moves: SimMove[]): number => {
    if (moves.length === 0) return -1;
    const power = g.powerSuit;
    const attacks: number[] = [], covers: number[] = [], passes: number[] = [];
    const goods: number[] = [], pickups: number[] = [];
    for (let i = 0; i < moves.length; i++) {
        switch (moves[i].type) {
            case MOVE_ATTACK: attacks.push(i); break;
            case MOVE_COVER:  covers.push(i); break;
            case MOVE_PASS:   passes.push(i); break;
            case MOVE_GOOD:   goods.push(i); break;
            case MOVE_PICKUP: pickups.push(i); break;
        }
    }

    if (attacks.length > 0) {
        const nonTrump = attacks.filter(i => moveAllNonTrump(moves[i], power));
        const trump = attacks.filter(i => moveHasTrump(moves[i], power));
        let candidates: number[] | null = null;
        if (nonTrump.length > 0) candidates = nonTrump;
        else if (trump.length > 0) {
            if (rngNext() < trumpAttackProbability(g)) candidates = trump;
            else if (goods.length > 0) return goods[0];
        }
        if (candidates) return pickMaxCardsLowestScore(moves, power, candidates);
    }

    if (passes.length > 0) {
        let best = passes[0], bestScore = Infinity;
        for (const i of passes) {
            const s = sumScore(moves[i], power);
            if (s < bestScore) { bestScore = s; best = i; }
        }
        return best;
    }

    if (covers.length > 0) {
        const uncovered = countUncovered(g);
        const full = covers.filter(i => moves[i].cards.length === uncovered);
        if (full.length > 0) {
            let best = full[0], bestScore = Infinity;
            for (const i of full) {
                let s = 1;
                for (const c of moves[i].cards) s *= cardScore(c, power);
                if (s < bestScore) { bestScore = s; best = i; }
            }
            return best;
        }
    }

    if (goods.length > 0) {
        let idx = Math.floor(rngNext() * goods.length);
        if (idx < 0) idx = 0;
        if (idx >= goods.length) idx = goods.length - 1;
        return goods[idx];
    }

    if (attacks.length > 0) {
        if (g.deck.length > 0 || g.flipped !== NONE) {
            const nt = attacks.filter(i => moveAllNonTrump(moves[i], power));
            if (nt.length > 0) return pickMaxCardsLowestScore(moves, power, nt);
            if (goods.length > 0) return goods[0];
        }
        return pickMaxCardsLowestScore(moves, power, attacks);
    }

    if (pickups.length > 0) return pickups[0];

    let idx = Math.floor(rngNext() * moves.length);
    if (idx < 0) idx = 0;
    if (idx >= moves.length) idx = moves.length - 1;
    return idx;
};

// ---------- direct rollout chooser (TASK A) -------------------------------
// handwrittenRolloutChoose produces the *identical* move that
// handwrittenChoose would pick from calcLegal(g, pi, lite=true), but WITHOUT
// enumerating the full combination list first. Returns the chosen SimMove, or
// null to defer to the slow enumerate-then-pick path (the reference for the
// branches it declines: trump-only/RNG-gated attacks and the espresso 1v1
// endgame). It consumes rngNext() exactly where handwritten does (the GOOD
// branch draws one), so the whole rollout RNG stream is unchanged.
// Ported from handwritten_rollout_choose in handwritten_strategy.c.
// Reusable scratch for handwrittenRolloutChoose (called millions of times per
// decision). The returned SimMove is consumed immediately by applyMove() and
// never retained, so a single module-level move object + card buffers are
// safe and avoid a per-ply object/array allocation. Parallel arrays replace
// the old array-of-{c,score,idx} objects.
const HR_MOVE: SimMove = { type: MOVE_GOOD, cards: [], attackCards: [] };
const hrCards: number[] = [];      // backing buffer for HR_MOVE.cards
const hrAtk: number[] = [];        // backing buffer for HR_MOVE.attackCards
const hrPoolC: number[] = [];      // pool card values
const hrPoolS: number[] = [];      // pool scores
const hrPoolI: number[] = [];      // pool hand indices
const hrOrder: number[] = [];      // index permutation for sorting the pool

// Emit HR_MOVE with the k lowest-score pool entries (ties by hand index), the
// chosen cards re-sorted by hand index — reproduces combinations() index-lex
// order + pickMaxCardsLowestScore. Reuses hrCards; returns HR_MOVE.
const hrEmitLowestK = (type: number, n: number, k: number): SimMove => {
    // Selection by (score asc, idx asc) without allocating: order[] is a
    // 0..n-1 permutation sorted by the pool keys, then we take the first k and
    // re-sort those by idx. n is tiny (a hand subset), so an insertion sort is
    // both allocation-free and fast.
    for (let i = 0; i < n; i++) hrOrder[i] = i;
    for (let i = 1; i < n; i++) {
        const oi = hrOrder[i];
        const si = hrPoolS[oi], di = hrPoolI[oi];
        let j = i - 1;
        while (j >= 0) {
            const oj = hrOrder[j];
            if (hrPoolS[oj] < si || (hrPoolS[oj] === si && hrPoolI[oj] <= di)) break;
            hrOrder[j + 1] = hrOrder[j]; j--;
        }
        hrOrder[j + 1] = oi;
    }
    // chosen = first k of order; re-sort those k by hand index into hrCards.
    hrCards.length = k;
    for (let i = 0; i < k; i++) hrCards[i] = hrPoolI[hrOrder[i]];   // temp: store idx
    // insertion sort hrCards (indices) ascending
    for (let i = 1; i < k; i++) {
        const v = hrCards[i]; let j = i - 1;
        while (j >= 0 && hrCards[j] > v) { hrCards[j + 1] = hrCards[j]; j--; }
        hrCards[j + 1] = v;
    }
    // map chosen hand-indices back to card values (need reverse lookup: build a
    // small map from idx->card by scanning the pool; n is tiny).
    for (let i = 0; i < k; i++) {
        const wantIdx = hrCards[i];
        for (let p = 0; p < n; p++) if (hrPoolI[p] === wantIdx) { hrCards[i] = hrPoolC[p]; break; }
    }
    HR_MOVE.type = type;
    HR_MOVE.cards = hrCards;
    hrAtk.length = 0;
    HR_MOVE.attackCards = hrAtk;
    return HR_MOVE;
};

const handwrittenRolloutChoose = (g: SimGame, pIdx: number): SimMove | null => {
    CDPROF.fastChoose++;
    if (g.over) return null;
    const hand = g.hands[pIdx];
    const power = g.powerSuit;
    const isDef = pIdx === g.defender;
    const firstAttack = g.battlesA.length === 0;
    const defenderCards = g.hands[g.defender].length;
    let uncovered = 0;
    for (let i = 0; i < g.battlesD.length; i++) if (g.battlesD[i] === NONE) uncovered++;

    // ---- Attacker: first attack ----
    if (firstAttack && pIdx === g.firstAttacker) {
        let bestVnt = -1, bestKnt = 0;
        for (let v = 0; v <= 13; v++) {
            let nt = 0;
            for (const c of hand) if ((c & 15) === v && (c >> 4) !== power) nt++;
            if (nt > 0) {
                let k = nt; if (k > defenderCards) k = defenderCards;
                if (k >= 1 && k > bestKnt) { bestKnt = k; bestVnt = v; }
            }
        }
        if (bestVnt >= 0) {
            let n = 0;
            for (let i = 0; i < hand.length; i++) {
                const c = hand[i];
                if ((c & 15) === bestVnt && (c >> 4) !== power) {
                    hrPoolC[n] = c; hrPoolS[n] = cardScore(c, power); hrPoolI[n] = i; n++;
                }
            }
            return hrEmitLowestK(MOVE_ATTACK, n, bestKnt);
        }
        // Trump-only first attack is RNG-gated by handwritten — defer.
        return null;
    }

    // ---- Defender ----
    if (isDef && g.battlesA.length > 0) {
        // Pass branch first (handwritten evaluates PASS before COVER).
        let anyCov = false;
        for (let i = 0; i < g.battlesD.length; i++) if (g.battlesD[i] !== NONE) anyCov = true;
        if (!anyCov) {
            const v0 = g.battlesA[0] & 15;
            let same = true;
            for (let i = 1; i < g.battlesA.length; i++) if ((g.battlesA[i] & 15) !== v0) same = false;
            if (same) {
                let n = 0;
                for (let i = 0; i < hand.length; i++) {
                    const c = hand[i];
                    if ((c & 15) === v0) {
                        hrPoolC[n] = c; hrPoolS[n] = cardScore(c, power); hrPoolI[n] = i; n++;
                    }
                }
                if (n > 0) {
                    const nextCards = g.hands[nextPlayer(g, g.defender)].length;
                    // Smallest legal k is k=1 if legal (legality tightens with k);
                    // min summed score = the single lowest matching card.
                    if (nextCards >= 1 + g.battlesA.length) {
                        return hrEmitLowestK(MOVE_PASS, n, 1);
                    }
                }
            }
        }
        // Greedy lowest-score full cover (matches calcCoverGreedy).
        hrAtk.length = 0;            // unc (attackCards)
        hrCards.length = 0;         // covers
        let full = true;
        for (let ai = 0; ai < g.battlesA.length; ai++) {
            if (g.battlesD[ai] !== NONE) continue;
            const a = g.battlesA[ai];
            hrAtk.push(a);
            let best = NONE, bestScore = Infinity;
            for (const c of hand) {
                // already-used check: skip cards already chosen as a cover
                let usedC = false;
                for (let u = 0; u < hrCards.length; u++) if (hrCards[u] === c) { usedC = true; break; }
                if (usedC) continue;
                if (canCoverInt(a, c, power)) {
                    const s = cardScore(c, power);
                    if (s < bestScore) { bestScore = s; best = c; }
                }
            }
            if (best === NONE) { full = false; break; }
            hrCards.push(best);
        }
        if (full) {
            HR_MOVE.type = MOVE_COVER; HR_MOVE.cards = hrCards; HR_MOVE.attackCards = hrAtk;
            return HR_MOVE;
        }
        HR_MOVE.type = MOVE_PICKUP; hrCards.length = 0; HR_MOVE.cards = hrCards;
        hrAtk.length = 0; HR_MOVE.attackCards = hrAtk;
        return HR_MOVE;
    }

    // ---- Attacker: regular (additional) attack ----
    if (!isDef && g.battlesA.length > 0) {
        if (g.goodMask & (1 << pIdx)) return null;  // no moves; slow path -1
        // table values present (small set — linear membership over battlesA).
        const cap = defenderCards - uncovered;
        // The slow regular-attack enumeration is capped at MAX_SOLVE_MOVES (96):
        // with >=7 valid cards it can produce >96 combos and TRUNCATE before the
        // largest, so handwritten may not see the true max-cards combo. Defer to
        // the slow path whenever truncation is possible (2^n-1 > 96, i.e. n>=7)
        // to stay behavior-identical. With <=6 valid cards (<=63 combos) the
        // enumeration never truncates and the direct max-combo is exact.
        const tableHasVal = (v: number): boolean => {
            for (let i = 0; i < g.battlesA.length; i++) {
                if ((g.battlesA[i] & 15) === v) return true;
                if (g.battlesD[i] !== NONE && (g.battlesD[i] & 15) === v) return true;
            }
            return false;
        };
        let nValid = 0;
        for (const c of hand) if (tableHasVal(c & 15)) nValid++;
        if (nValid >= 7) return null;
        let n = 0, nTr = 0;
        for (let i = 0; i < hand.length; i++) {
            const c = hand[i];
            if (!tableHasVal(c & 15)) continue;
            if ((c >> 4) === power) nTr++;
            else { hrPoolC[n] = c; hrPoolS[n] = cardScore(c, power); hrPoolI[n] = i; n++; }
        }
        if (n > 0 && cap >= 1) {
            let k = n; if (k > cap) k = cap;
            return hrEmitLowestK(MOVE_ATTACK, n, k);
        }
        // No legal non-trump attack. If no legal trump attack either, handwritten
        // falls straight through to GOOD (drawing one rngNext() to index among
        // GOOD moves — always exactly one here). Consume the identical draw.
        const haveTrumpAttack = nTr > 0 && cap >= 1;
        if (!haveTrumpAttack) {
            rngNext();
            HR_MOVE.type = MOVE_GOOD; hrCards.length = 0; HR_MOVE.cards = hrCards;
            hrAtk.length = 0; HR_MOVE.attackCards = hrAtk;
            return HR_MOVE;
        }
        return null;  // trump attack exists: RNG-gated, defer to slow path.
    }

    return null;
};

// ---------- rollout policy: espresso (port of espresso_strategy.c) --------
// Inside a sampled world, espresso's hand-reading is our own guess, not real
// hidden state — same legitimacy argument as the C bots. Used only for
// multi-player deck-empty endgame rollouts.

const getOpponentIdx = (g: SimGame, pIdx: number): number => {
    let inOpps = 0;
    for (let i = 0; i < g.numPlayers; i++) {
        if (i !== pIdx && g.pStatus[i] === ST_IN) inOpps++;
    }
    if (inOpps === 0) return -1;
    if (g.defender !== pIdx && g.pStatus[g.defender] === ST_IN) return g.defender;
    if (g.firstAttacker !== pIdx && g.pStatus[g.firstAttacker] === ST_IN) return g.firstAttacker;
    for (let i = 0; i < g.numPlayers; i++) {
        if (i !== pIdx && g.pStatus[i] === ST_IN) return i;
    }
    return -1;
};

const predictCover = (attacks: number[], oppHand: number[], power: number):
        { covers: number[], pickup: boolean } => {
    const remaining = oppHand.slice();
    const covers: number[] = [];
    for (const a of attacks) {
        let bestIdx = -1, bestScore = Infinity;
        for (let i = 0; i < remaining.length; i++) {
            if (canCoverInt(a, remaining[i], power)) {
                const s = cardScore(remaining[i], power);
                if (s < bestScore) { bestScore = s; bestIdx = i; }
            }
        }
        if (bestIdx < 0) return { covers: [], pickup: true };
        covers.push(remaining[bestIdx]);
        remaining.splice(bestIdx, 1);
    }
    return { covers, pickup: false };
};

const espressoRolloutRound = (firstAttack: number[], myHand: number[], oppHand: number[],
        power: number): { my: number[], opp: number[], pickup: boolean } => {
    let my = myHand.slice();
    let opp = oppHand.slice();
    const tableValues = new Set<number>();
    let attack = firstAttack.slice();

    for (let iter = 0; iter < 5; iter++) {
        my = my.filter(c => attack.indexOf(c) < 0);
        for (const c of attack) tableValues.add(c & 15);

        const pc = predictCover(attack, opp, power);
        if (pc.pickup) {
            opp = opp.concat(attack);
            return { my, opp, pickup: true };
        }
        opp = opp.filter(c => pc.covers.indexOf(c) < 0);
        for (const c of pc.covers) tableValues.add(c & 15);

        const matching = my.filter(c => tableValues.has(c & 15) && (c >> 4) !== power);
        if (matching.length === 0) return { my, opp, pickup: false };
        const seenV = new Set<number>();
        let bestGroup: number[] = [], bestSum = Infinity;
        for (const c of matching) {
            const v = c & 15;
            if (seenV.has(v)) continue;
            seenV.add(v);
            const group = matching.filter(m => (m & 15) === v);
            const sum = group.reduce((s, x) => s + (x & 15), 0);
            const better = group.length > bestGroup.length
                || (group.length === bestGroup.length && sum < bestSum);
            if (better) { bestGroup = group; bestSum = sum; }
        }
        attack = bestGroup.slice(0, Math.min(bestGroup.length, opp.length));
        if (attack.length === 0) return { my, opp, pickup: false };
    }
    return { my, opp, pickup: false };
};

const espressoChoose = (g: SimGame, pIdx: number, moves: SimMove[]): number => {
    if (moves.length === 0) return -1;
    let inC = 0;
    for (let i = 0; i < g.numPlayers; i++) if (g.pStatus[i] === ST_IN) inC++;
    if (inC > 2) return handwrittenChoose(g, pIdx, moves);

    const power = g.powerSuit;
    const myHand = g.hands[pIdx];
    const oppIdx = getOpponentIdx(g, pIdx);
    const oppHand = oppIdx >= 0 ? g.hands[oppIdx] : null;

    const isNonTrumpAttack = (m: SimMove): boolean =>
        m.type === MOVE_ATTACK && m.cards.every(c => (c >> 4) !== power);
    const isTrumpAttack = (m: SimMove): boolean =>
        m.type === MOVE_ATTACK && m.cards.some(c => (c >> 4) === power);

    let nAttack = 0, nNonTrump = 0, nTrump = 0;
    for (const m of moves) {
        if (m.type === MOVE_ATTACK) {
            nAttack++;
            if (isNonTrumpAttack(m)) nNonTrump++;
            else if (isTrumpAttack(m)) nTrump++;
        }
    }

    const candidateIdx: number[] = [];
    if (nAttack > 0) {
        if (nNonTrump > 0) {
            for (let i = 0; i < moves.length; i++) if (isNonTrumpAttack(moves[i])) candidateIdx.push(i);
        } else if (nTrump > 0) {
            let myLowestTrump = ACE + 1;
            for (const m of moves) {
                if (!isTrumpAttack(m)) continue;
                for (const c of m.cards) {
                    if ((c >> 4) === power && (c & 15) < myLowestTrump) myLowestTrump = c & 15;
                }
            }
            let allow = false;
            if (oppHand) {
                for (const c of oppHand) {
                    if ((c >> 4) === power && (c & 15) > myLowestTrump) { allow = true; break; }
                }
            }
            if (allow || rngNext() < trumpAttackProbability(g)) {
                for (let i = 0; i < moves.length; i++) if (isTrumpAttack(moves[i])) candidateIdx.push(i);
            } else {
                for (let i = 0; i < moves.length; i++) if (moves[i].type === MOVE_GOOD) return i;
            }
        }
    }

    if (candidateIdx.length > 0) {
        let minOppHand = Infinity, minOppIdx = -1;
        for (let i = 0; i < g.numPlayers; i++) {
            if (i === pIdx || g.pStatus[i] !== ST_IN) continue;
            if (g.hands[i].length < minOppHand) { minOppHand = g.hands[i].length; minOppIdx = i; }
        }
        const defenderIsLeader = g.defender !== pIdx && g.pStatus[g.defender] === ST_IN
            && g.hands[g.defender].length === minOppHand;
        const leaderIsAttacker = minOppIdx >= 0 && minOppIdx !== g.defender;
        const leaderHand = minOppIdx >= 0 ? g.hands[minOppIdx] : null;
        const deckActive = g.deck.length > 0 || g.flipped !== NONE;

        let passWindow = oppHand !== null;
        for (let i = 0; i < g.battlesD.length; i++) {
            if (g.battlesD[i] !== NONE) { passWindow = false; break; }
        }

        let best = candidateIdx[0];
        let bestEval = -Infinity, bestCount = -1, bestSum = Infinity;
        let first = true;
        for (const ci of candidateIdx) {
            const m = moves[ci];
            const v = m.cards[0] & 15;
            let passable = false;
            if (passWindow && oppHand) {
                for (const c of oppHand) if ((c & 15) === v) { passable = true; break; }
            }
            let e = 0;
            if (oppHand) {
                const r = espressoRolloutRound(m.cards, myHand, oppHand, power);
                let myT = 0; for (const c of r.my) if ((c >> 4) === power) myT++;
                let oppT = 0; for (const c of r.opp) if ((c >> 4) === power) oppT++;
                const sizeWeight = (r.pickup || !deckActive) ? 1 : 0;
                const pickupBonus = r.pickup ? 3 : 0;
                const blockLeader = (r.pickup && defenderIsLeader) ? 4 : 0;
                let leaderPileOnPen = 0;
                if (leaderIsAttacker && leaderHand) {
                    const myV = m.cards[0] & 15;
                    let matches = 0;
                    for (const c of leaderHand) if ((c & 15) === myV) matches++;
                    leaderPileOnPen = matches * 0.7;
                }
                e = sizeWeight * (r.opp.length - r.my.length)
                  + 1.5 * (myT - oppT)
                  + pickupBonus + blockLeader - leaderPileOnPen;
            }
            const finalEval = passable ? e - 1000 : e;
            const cnt = m.cards.length;
            const sum = sumScore(m, power);
            const take = first
                || finalEval > bestEval
                || (finalEval === bestEval && cnt > bestCount)
                || (finalEval === bestEval && cnt === bestCount && sum < bestSum);
            if (take) { best = ci; bestEval = finalEval; bestCount = cnt; bestSum = sum; first = false; }
        }
        return best;
    }

    // ---- pass moves -----------------------------------------------------
    const passIdx: number[] = [];
    for (let i = 0; i < moves.length; i++) if (moves[i].type === MOVE_PASS) passIdx.push(i);
    if (passIdx.length > 0 && oppHand) {
        let best = passIdx[0], bestEval = -Infinity, first = true;
        for (const pi of passIdx) {
            const m = moves[pi];
            const allAttacks: number[] = [];
            for (let i = 0; i < g.battlesA.length; i++) allAttacks.push(g.battlesA[i]);
            for (const c of m.cards) allAttacks.push(c);
            const pc = predictCover(allAttacks, oppHand, power);
            const myH = myHand.filter(c => m.cards.indexOf(c) < 0);
            let oppH: number[];
            if (pc.pickup) oppH = oppHand.concat(allAttacks);
            else oppH = oppHand.filter(c => pc.covers.indexOf(c) < 0);
            let myT = 0; for (const c of myH) if ((c >> 4) === power) myT++;
            let oppT = 0; for (const c of oppH) if ((c >> 4) === power) oppT++;
            const deckActive = g.deck.length > 0 || g.flipped !== NONE;
            const sizeWeight = (pc.pickup || !deckActive) ? 1 : 0;
            const e = sizeWeight * (oppH.length - myH.length) + 1.5 * (myT - oppT) + (pc.pickup ? 3 : 0);
            if (first || e > bestEval) { bestEval = e; best = pi; first = false; }
        }
        return best;
    }
    if (passIdx.length > 0) return passIdx[0];

    // ---- cover moves ------------------------------------------------------
    const coverIdx: number[] = [];
    for (let i = 0; i < moves.length; i++) if (moves[i].type === MOVE_COVER) coverIdx.push(i);
    if (coverIdx.length > 0) {
        const uncovered = countUncovered(g);
        const fullIdx = coverIdx.filter(i => moves[i].cards.length === uncovered);
        if (fullIdx.length > 0) {
            // stillInPlay = unseen deck pool (via discard memory) + opp hands.
            const known = new Set<number>();
            for (let i = 0; i < g.numPlayers; i++) for (const c of g.hands[i]) known.add(c);
            for (let i = 0; i < g.battlesA.length; i++) {
                known.add(g.battlesA[i]);
                if (g.battlesD[i] !== NONE) known.add(g.battlesD[i]);
            }
            if (g.flipped !== NONE) known.add(g.flipped);
            for (const c of g.discards) known.add(c);
            const startV = g.numPlayers > 4 ? 1 : 5;
            const still: number[] = [];
            for (let suit = 0; suit < 4; suit++) {
                for (let v = startV; v <= 14; v++) {     // TS uses 14 — kept verbatim
                    const c = mkCard(suit, v);
                    if (!known.has(c)) still.push(c);
                }
            }
            for (let i = 0; i < g.numPlayers; i++) {
                if (i === pIdx || g.pStatus[i] !== ST_IN) continue;
                for (const c of g.hands[i]) still.push(c);
            }

            const tableV = new Set<number>();
            for (let i = 0; i < g.battlesA.length; i++) {
                tableV.add(g.battlesA[i] & 15);
                if (g.battlesD[i] !== NONE) tableV.add(g.battlesD[i] & 15);
            }
            let allOppTrumps = 0;
            for (let i = 0; i < g.numPlayers; i++) {
                if (i === pIdx || g.pStatus[i] !== ST_IN) continue;
                for (const c of g.hands[i]) if ((c >> 4) === power) allOppTrumps++;
            }

            let best = fullIdx[0], bestEval = -Infinity, bestMax = Infinity, bestSum = Infinity;
            let first = true;
            for (const fi of fullIdx) {
                const m = moves[fi];
                const remaining = myHand.filter(c => m.cards.indexOf(c) < 0);
                let myTrumpsAfter = 0;
                for (const c of remaining) if ((c >> 4) === power) myTrumpsAfter++;
                let defendable = 0;
                for (let i = 0; i < g.numPlayers; i++) {
                    if (i === pIdx || g.pStatus[i] !== ST_IN) continue;
                    for (const oc of g.hands[i]) {
                        for (const rc of remaining) {
                            if (canCoverInt(oc, rc, power)) { defendable++; break; }
                        }
                    }
                }
                let disposedUtility = 0, pileOn = 0;
                for (const c of m.cards) {
                    let n = 0;
                    for (const t of still) if (canCoverInt(t, c, power)) n++;
                    disposedUtility += n;
                    if (!tableV.has(c & 15)) {
                        for (let p2 = 0; p2 < g.numPlayers; p2++) {
                            if (p2 === pIdx || g.pStatus[p2] !== ST_IN || p2 === g.defender) continue;
                            for (const oc of g.hands[p2]) if ((oc & 15) === (c & 15)) pileOn++;
                        }
                    }
                }
                const e = defendable * 0.5
                    + 1.5 * (myTrumpsAfter - allOppTrumps)
                    - 0.3 * disposedUtility
                    - 1.0 * pileOn;
                let mx = 0, sm = 0;
                for (const c of m.cards) {
                    const sc = cardScore(c, power);
                    if (sc > mx) mx = sc;
                    sm += sc;
                }
                const take = first
                    || e > bestEval
                    || (e === bestEval && mx < bestMax)
                    || (e === bestEval && mx === bestMax && sm < bestSum);
                if (take) { best = fi; bestEval = e; bestMax = mx; bestSum = sm; first = false; }
            }
            return best;
        }
    }

    // ---- good -------------------------------------------------------------
    for (let i = 0; i < moves.length; i++) if (moves[i].type === MOVE_GOOD) return i;

    // ---- done attacks (count desc, score asc) ------------------------------
    const doneIdx: number[] = [];
    for (let i = 0; i < moves.length; i++) if (moves[i].type === MOVE_ATTACK) doneIdx.push(i);
    if (doneIdx.length > 0) {
        let best = doneIdx[0];
        let bestCount = moves[best].cards.length;
        let bestSum = sumScore(moves[best], power);
        for (let i = 1; i < doneIdx.length; i++) {
            const m = moves[doneIdx[i]];
            const sm = sumScore(m, power);
            const take = m.cards.length > bestCount || (m.cards.length === bestCount && sm < bestSum);
            if (take) { best = doneIdx[i]; bestCount = m.cards.length; bestSum = sm; }
        }
        return best;
    }

    for (let i = 0; i < moves.length; i++) if (moves[i].type === MOVE_PICKUP) return i;

    let idx = Math.floor(rngNext() * moves.length);
    if (idx < 0) idx = 0;
    if (idx >= moves.length) idx = moves.length - 1;
    return idx;
};

// ---------- rollout ------------------------------------------------------

const rolloutPolicyFor = (g: SimGame): ((g: SimGame, p: number, m: SimMove[]) => number) => {
    const deckActive = g.deck.length > 0 || g.flipped !== NONE;
    if (deckActive || inCount(g) === 2) return handwrittenChoose;
    return espressoChoose;
};

// Roll a sampled world forward; returns my finish position (1..N), or 0 if
// the simulation didn't terminate. Exits early once my position is known.
const simulate = (g: SimGame, myIdx: number, maxTurns: number): number => {
    CDPROF.simulate++;
    let turns = 0;
    while (gameDone(g) < 0 && turns++ < maxTurns) {
        CDPROF.simTurns++;
        if (g.over) break;
        if (g.pStatus[myIdx] !== ST_IN) {
            const pos = g.elim.indexOf(myIdx);
            if (pos >= 0) return pos + 1;
            break;
        }
        let acted = false;
        // The rollout policy resolves to pure handwritten everywhere EXCEPT the
        // espresso 1v1 endgame (deck dead AND exactly 2 players in). The direct
        // chooser (TASK A) skips per-ply combination enumeration in that common
        // case; it defers (returns null) on the trump-gated / espresso-1v1
        // branches, which fall through to the slow enumerate-then-pick path.
        const deckActive = g.deck.length > 0 || g.flipped !== NONE;
        const useHw = (deckActive || inCount(g) !== 2) && !noFastroll();
        for (let pi = 0; pi < g.numPlayers; pi++) {
            if (!shouldAct(g, pi)) continue;
            if (useHw) {
                const fm = handwrittenRolloutChoose(g, pi);
                if (fm !== null) {
                    if (applyMove(g, pi, fm)) { acted = true; break; }
                    // unexpectedly illegal: fall through to slow path.
                }
            }
            const moves = calcLegal(g, pi, true);
            if (moves.length === 0) continue;
            const policy = rolloutPolicyFor(g);
            const idx = policy(g, pi, moves);
            if (idx < 0 || idx >= moves.length) continue;
            if (applyMove(g, pi, moves[idx])) { acted = true; break; }
        }
        if (!acted) break;
    }
    if (gameDone(g) < 0) return 0;
    const pos = g.elim.indexOf(myIdx);
    if (pos >= 0) return pos + 1;
    return g.numPlayers;
};

// ---------- belief -------------------------------------------------------

const MAX_VOIDS = 6;

// One entry of the public move history, pre-converted by the adapter.
// Cards are ints; masked/unknown cards are NONE.
export interface BeliefLog {
    type: 'game_start' | 'attack' | 'cover' | 'pass' | 'pickup' | 'good'
        | 'discard' | 'defender_change' | 'player_out' | 'draw';
    playerIdx: number;          // -1 for system events
    pairs: { primary: number, target: number }[];  // target NONE unless cover
}

export interface PublicView {
    numPlayers: number;
    powerSuit: number;
    firstAttacker: number;
    defender: number;
    deckCount: number;
    discardLen: number;
    flipped: number;            // NONE if already taken
    battlesA: number[];
    battlesD: number[];
    myIdx: number;
    myHand: number[];
    handCounts: number[];
    statuses: number[];         // ST_IN / ST_OUT (0/1)
    goodMask: number;
    logs: BeliefLog[];
}

interface Belief {
    pool: number[];
    pinned: number[][];
    voids: number[][];
    floorV: number[];
}

const voidForbidden = (B: Belief, power: number, p: number, c: number): boolean => {
    for (const v of B.voids[p]) if (canCoverInt(v, c, power)) return true;
    return false;
};
const floorForbidden = (B: Belief, power: number, p: number, c: number): boolean =>
    B.floorV[p] > 0 && (c >> 4) !== power && (c & 15) < B.floorV[p];

const buildBelief = (pv: PublicView): Belief => {
    const n = pv.numPlayers;
    const B: Belief = {
        pool: [],
        pinned: Array.from({ length: n }, () => [] as number[]),
        voids: Array.from({ length: n }, () => [] as number[]),
        floorV: new Array(n).fill(0),
    };
    const power = pv.powerSuit;
    const distrustFloor = new Array(n).fill(false);
    const trumpViol = new Array(n).fill(0);

    let lastDrawIdx = -1;
    for (let i = 0; i < pv.logs.length; i++) if (pv.logs[i].type === 'draw') lastDrawIdx = i;
    const deckAliveNow = pv.deckCount > 0 || pv.flipped !== NONE;

    const pinnedAdd = (p: number, c: number): void => {
        if (c === NONE) return;
        if (B.pinned[p].indexOf(c) < 0) B.pinned[p].push(c);
    };
    const pinnedRemove = (p: number, c: number): void => {
        const i = B.pinned[p].indexOf(c);
        if (i >= 0) B.pinned[p].splice(i, 1);
    };
    const floorCheck = (p: number, c: number): void => {
        if (p < 0 || B.floorV[p] <= 0 || c === NONE) return;
        if ((c >> 4) !== power && (c & 15) < B.floorV[p]) {
            B.floorV[p] = 0;
            distrustFloor[p] = true;
        }
    };

    let inNow = n;
    const tbl: number[] = [];
    const unc: number[] = [];
    const discards: number[] = [];

    for (let i = 0; i < pv.logs.length; i++) {
        const L = pv.logs[i];
        const p = L.playerIdx;
        const deckAliveAt = deckAliveNow || i <= lastDrawIdx;
        switch (L.type) {
            case 'attack':
            case 'pass': {
                const firstAttack = L.type === 'attack' && tbl.length === 0;
                let anyTrump = false;
                for (const pair of L.pairs) {
                    const c = pair.primary;
                    if (c === NONE) continue;
                    if ((c >> 4) === power) anyTrump = true;
                    unc.push(c);
                    tbl.push(c);
                    if (p >= 0 && p !== pv.myIdx) {
                        floorCheck(p, c);
                        pinnedRemove(p, c);
                    }
                }
                if (p >= 0 && p !== pv.myIdx && L.type === 'attack') {
                    if (anyTrump && deckAliveAt) trumpViol[p]++;
                    if (firstAttack && L.pairs.length === 1 && !anyTrump && inNow > 2) {
                        B.floorV[p] = L.pairs[0].primary & 15;
                    }
                }
                break;
            }
            case 'cover':
                for (const pair of L.pairs) {
                    const c = pair.primary;
                    if (c === NONE) continue;
                    tbl.push(c);
                    if (p >= 0 && p !== pv.myIdx) {
                        floorCheck(p, c);
                        pinnedRemove(p, c);
                    }
                    if (pair.target !== NONE) {
                        const q = unc.indexOf(pair.target);
                        if (q >= 0) { unc[q] = unc[unc.length - 1]; unc.pop(); }
                    }
                }
                break;
            case 'pickup':
                if (p >= 0 && p !== pv.myIdx) {
                    if (unc.length === 1 && B.voids[p].length < MAX_VOIDS) {
                        B.voids[p].push(unc[0]);
                    }
                    for (const c of tbl) pinnedAdd(p, c);
                    B.floorV[p] = 0;
                }
                tbl.length = 0;
                unc.length = 0;
                break;
            case 'discard':
                for (const c of tbl) discards.push(c);
                tbl.length = 0;
                unc.length = 0;
                break;
            case 'draw':
                if (p >= 0 && p !== pv.myIdx) {
                    B.voids[p].length = 0;
                    B.floorV[p] = 0;
                    // TS draw logs mask cards as NONE — except the flipped
                    // trump, which is publicly visible when taken. Pin it.
                    for (const pair of L.pairs) {
                        if (pair.primary !== NONE) pinnedAdd(p, pair.primary);
                    }
                }
                break;
            case 'player_out':
                inNow--;
                break;
            default:
                break;
        }
    }

    // Behavior-based trust: lowest-first attackers almost never lead trump
    // while the deck is alive. One strike kills floors (voids stay; the
    // 1-in-4 unconstrained world mixture absorbs void violators).
    for (let p = 0; p < n; p++) {
        if (trumpViol[p] >= 1) { distrustFloor[p] = true; B.floorV[p] = 0; }
    }

    for (let p = 0; p < n; p++) {
        if (p === pv.myIdx || pv.statuses[p] !== ST_IN) {
            B.pinned[p].length = 0;
            B.voids[p].length = 0;
            B.floorV[p] = 0;
            continue;
        }
        if (B.pinned[p].length > pv.handCounts[p]) {
            B.pinned[p].length = pv.handCounts[p];   // defensive clamp
        }
    }

    // Unseen pool = full deck minus everything publicly located.
    const known = new Set<number>();
    for (const c of pv.myHand) known.add(c);
    for (let i = 0; i < pv.battlesA.length; i++) {
        known.add(pv.battlesA[i]);
        if (pv.battlesD[i] !== NONE) known.add(pv.battlesD[i]);
    }
    if (pv.flipped !== NONE) known.add(pv.flipped);
    for (const c of discards) known.add(c);
    for (let p = 0; p < n; p++) for (const c of B.pinned[p]) known.add(c);

    const startV = minValueFor(n);
    for (let suit = 0; suit < 4; suit++) {
        for (let v = startV; v <= ACE; v++) {
            const c = mkCard(suit, v);
            if (!known.has(c)) B.pool.push(c);
        }
    }

    // Feasibility: relax floors first, then voids.
    for (let p = 0; p < n; p++) {
        if (B.voids[p].length === 0 && B.floorV[p] === 0) continue;
        const unknown = pv.handCounts[p] - B.pinned[p].length;
        if (unknown <= 0) continue;
        let allowed = 0;
        for (const c of B.pool) {
            if (!voidForbidden(B, power, p, c) && !floorForbidden(B, power, p, c)) allowed++;
        }
        if (allowed < unknown && B.floorV[p] > 0) {
            B.floorV[p] = 0;
            allowed = 0;
            for (const c of B.pool) if (!voidForbidden(B, power, p, c)) allowed++;
        }
        if (allowed < unknown) B.voids[p].length = 0;
    }

    return B;
};

// ---------- world sampling -------------------------------------------------

const sampleWorld = (pv: PublicView, B: Belief, seed: number,
        applyVoids: boolean, applyFloors: boolean): SimGame => {
    CDPROF.sampleWorld++;
    const n = pv.numPlayers;
    const g: SimGame = {
        over: false,
        numPlayers: n,
        powerSuit: pv.powerSuit,
        firstAttacker: pv.firstAttacker,
        defender: pv.defender,
        deck: [],
        flipped: pv.flipped,
        battlesA: pv.battlesA.slice(),
        battlesD: pv.battlesD.slice(),
        discardLen: pv.discardLen,
        pStatus: pv.statuses.slice(),
        hands: Array.from({ length: n }, () => [] as number[]),
        elim: [],
        goodMask: pv.goodMask,
        discards: [],
    };
    // Reconstruct elimination order from statuses is impossible publicly per
    // seat order — but rollout positions only need players ALREADY out to
    // occupy the first slots. The adapter passes the real public elimination
    // order via pv (player ids → indices) when available; we fall back to
    // status scan order. See makePublicView.
    for (const e of (pv as PublicView & { elimOrder?: number[] }).elimOrder ?? []) g.elim.push(e);

    g.hands[pv.myIdx] = pv.myHand.slice();
    for (let p = 0; p < n; p++) {
        if (p === pv.myIdx) continue;
        for (const c of B.pinned[p]) g.hands[p].push(c);
    }

    // Discard memory for rollout espresso: replay real discard logs.
    for (const L of pv.logs) {
        if (L.type === 'discard') {
            for (const pair of L.pairs) if (pair.primary !== NONE) g.discards.push(pair.primary);
        }
    }

    const hidden = B.pool.slice();
    if (hidden.length === 0) return g;
    let s = (seed >>> 0) || 0xCAFE;
    for (let i = hidden.length - 1; i > 0; i--) {
        s = xorshift(s);
        const j = s % (i + 1);
        const sw = hidden[i]; hidden[i] = hidden[j]; hidden[j] = sw;
    }

    let k = 0;
    const deckN = pv.deckCount;
    for (let i = 0; i < deckN && k < hidden.length; i++) g.deck.push(hidden[k++]);

    const slots: { player: number, slot: number }[] = [];
    for (let p = 0; p < n; p++) {
        if (p === pv.myIdx) continue;
        const need = pv.handCounts[p] - B.pinned[p].length;
        for (let j = 0; j < need && k < hidden.length; j++) {
            slots.push({ player: p, slot: g.hands[p].length });
            g.hands[p].push(hidden[k++]);
        }
    }

    if (!applyVoids && !applyFloors) return g;
    const power = pv.powerSuit;
    for (const sl of slots) {
        const p = sl.player;
        const useV = applyVoids && B.voids[p].length > 0;
        const useF = applyFloors && B.floorV[p] > 0;
        if (!useV && !useF) continue;
        const c = g.hands[p][sl.slot];
        const bad = (useV && voidForbidden(B, power, p, c))
                 || (useF && floorForbidden(B, power, p, c));
        if (!bad) continue;
        for (let d = 0; d < g.deck.length; d++) {
            const dc = g.deck[d];
            const dcBad = (useV && voidForbidden(B, power, p, dc))
                       || (useF && floorForbidden(B, power, p, dc));
            if (!dcBad) {
                g.deck[d] = c;
                g.hands[p][sl.slot] = dc;
                break;
            }
        }
    }
    return g;
};

// ---------- exact endgame solver --------------------------------------------

const SOLVE_MAX_DEPTH = 48;
const SOLVE_BUDGET = 200000;
const AVOID_BUDGET = 150000;
const SOLVE_MAX_CARDS = 20;

// Transposition table for the endgame solver. These 2-player deck-empty
// endgames transpose heavily (move orderings converge to the same position),
// so memoizing resolved subtrees is a large win — and on the latency-bounded
// TS path a faster solve frees wall-clock for more world sampling. Mirrors the
// C bitboard solver's TT: store EXACT values only (a fail-soft alpha-beta
// result is the true game value only when it lands strictly inside the
// original window — otherwise it is a bound and must not be memoized), and the
// stored value is depth-relative so it re-bases when the same position is
// reached at a different depth. Keyed on the value-relevant state (both hands,
// table, roles) as a string fingerprint (exact — no hash collisions).
interface Solver { budget: number; aborted: boolean; me: number; tt: Map<string, number>; }

// Reused scratch for sorting a hand inside ttFingerprint (called once per
// solver node — formerly allocated a .slice().sort() array each time).
const ttSortBuf: number[] = [];

const ttFingerprint = (g: SimGame): string => {
    let s = "";
    for (let p = 0; p < g.numPlayers; p++) {
        if (g.pStatus[p] !== ST_IN) continue;
        const h = g.hands[p];
        // copy+insertion-sort into the shared buffer (hands are tiny in the
        // endgame; allocation-free, identical sorted order to .sort((a,b)=>a-b)).
        const m = h.length;
        for (let i = 0; i < m; i++) ttSortBuf[i] = h[i];
        for (let i = 1; i < m; i++) {
            const v = ttSortBuf[i]; let j = i - 1;
            while (j >= 0 && ttSortBuf[j] > v) { ttSortBuf[j + 1] = ttSortBuf[j]; j--; }
            ttSortBuf[j + 1] = v;
        }
        s += p + ":";
        for (let i = 0; i < m; i++) s += (i ? "," : "") + ttSortBuf[i];
        s += "|";
    }
    s += "B";
    for (let i = 0; i < g.battlesA.length; i++) s += g.battlesA[i] + "." + g.battlesD[i] + ";";
    s += "d" + g.defender + "f" + g.firstAttacker + "g" + g.goodMask;
    return s;
};

// Pack a depth-relative value + depth into one number (value*256 + depth) so a
// single Map<string,number> suffices. value in [-1000,1000], depth in [0,255].
const ttEncode = (value: number, depth: number): number => value * 256 + depth;

const solve = (S: Solver, g: SimGame, alpha: number, beta: number, depth: number): number => {
    CDPROF.solveNodes++;
    const loser = gameDone(g);
    if (loser >= 0) return loser === S.me ? -(1000 - depth) : (1000 - depth);
    if (inCount(g) === 0) return 0;
    if (depth >= SOLVE_MAX_DEPTH) { S.aborted = true; return 0; }
    if (--S.budget <= 0) { S.aborted = true; return 0; }

    let actor = -1;
    if (shouldAct(g, g.defender)) actor = g.defender;
    else {
        for (let i = 0; i < g.numPlayers; i++) if (shouldAct(g, i)) { actor = i; break; }
    }
    if (actor < 0) return 0;

    const key = ttFingerprint(g);
    const hit = S.tt.get(key);
    if (hit !== undefined) {
        let v = Math.trunc(hit / 256);
        const sd = hit - v * 256;   // stored depth
        if (v > 0) v = v - (1000 - sd) + (1000 - depth);
        else if (v < 0) v = v + (1000 - sd) - (1000 - depth);
        return v;
    }

    const mv = calcLegal(g, actor, false);
    if (mv.length === 0) return 0;
    if (mv.length > MAX_SOLVE_MOVES) { S.aborted = true; return 0; }

    const alpha0 = alpha, beta0 = beta;
    const maximizing = actor === S.me;
    let best = maximizing ? -2000 : 2000;
    for (const m of mv) {
        const child = cloneSim(g);
        if (!applyMove(child, actor, m)) continue;
        const v = solve(S, child, alpha, beta, depth + 1);
        if (S.aborted) return 0;
        if (maximizing) {
            if (v > best) best = v;
            if (best > alpha) alpha = best;
        } else {
            if (v < best) best = v;
            if (best < beta) beta = best;
        }
        if (alpha >= beta) break;
    }
    if (best === -2000 || best === 2000) return 0;
    // Memoize EXACT values only (strictly inside the original window).
    if (best > alpha0 && best < beta0 && depth <= 255) {
        S.tt.set(key, ttEncode(best, depth));
    }
    return best;
};

// Solve the real position when 2 players remain and the deck is empty
// (unseen pool IS the opponent's hand — public deduction). Returns the
// fastest forced-win candidate index, or -1; marks proven-loss candidates.
const tryEndgameSolve = (pv: PublicView, B: Belief, candidates: SimMove[],
        forcedLoss: boolean[]): number => {
    if (pv.deckCount > 0 || pv.flipped !== NONE) return -1;
    let inC = 0;
    for (let i = 0; i < pv.numPlayers; i++) if (pv.statuses[i] === ST_IN) inC++;
    if (inC !== 2 || pv.statuses[pv.myIdx] !== ST_IN) return -1;

    let opp = -1;
    for (let i = 0; i < pv.numPlayers; i++) {
        if (i !== pv.myIdx && pv.statuses[i] === ST_IN) opp = i;
    }
    if (opp < 0) return -1;
    const unknown = pv.handCounts[opp] - B.pinned[opp].length;
    if (unknown < 0 || unknown !== B.pool.length) return -1;
    if (pv.myHand.length + pv.handCounts[opp] > SOLVE_MAX_CARDS) return -1;

    const root = sampleWorld(pv, B, 1, false, false);
    // sampleWorld already deals pool → opp unknown slots (deckCount = 0).

    const S: Solver = { budget: SOLVE_BUDGET, aborted: false, me: pv.myIdx, tt: new Map() };

    let bestIdx = -1, bestV = 0, alpha = 0;
    let anyAbort = false;
    for (let i = 0; i < candidates.length; i++) {
        const child = cloneSim(root);
        if (!applyMove(child, pv.myIdx, candidates[i])) continue;
        S.aborted = false;
        const v = solve(S, child, alpha, 2000, 1);
        if (S.budget <= 0) return -1;
        if (S.aborted) { anyAbort = true; continue; }
        if (v > bestV) { bestV = v; bestIdx = i; }
        if (v > alpha) alpha = v;
    }
    if (bestIdx >= 0) return bestIdx;
    if (anyAbort) return -1;

    // Loss avoidance: null-window classification; only restrict when some
    // move is PROVEN non-losing (else adverse selection — see CORDITE.md).
    S.budget = AVOID_BUDGET;
    let nLoss = 0, nNonLoss = 0;
    for (let i = 0; i < candidates.length; i++) {
        const child = cloneSim(root);
        if (!applyMove(child, pv.myIdx, candidates[i])) continue;
        S.aborted = false;
        const v = solve(S, child, -1, 0, 1);
        if (S.budget <= 0 || S.aborted) continue;
        if (v < 0) { forcedLoss[i] = true; nLoss++; }
        else nNonLoss++;
    }
    if (!(nLoss > 0 && nNonLoss > 0)) forcedLoss.fill(false);
    return -1;
};

// ---------- candidate selection ---------------------------------------------

const MAX_CANDS = 26;

const rankedInsert = (idxs: number[], keys: number[], cap: number,
        idx: number, key: number): void => {
    let pos = idxs.length;
    while (pos > 0 && keys[pos - 1] > key) pos--;
    if (pos >= cap) return;
    idxs.splice(pos, 0, idx);
    keys.splice(pos, 0, key);
    if (idxs.length > cap) { idxs.pop(); keys.pop(); }
};

const pickCandidates = (power: number, moves: SimMove[], excluded: boolean[]): number[] => {
    const atk: number[] = [], atkK: number[] = [];
    const cov: number[] = [], covK: number[] = [];
    const pas: number[] = [], pasK: number[] = [];
    let goodIdx = -1, pickupIdx = -1;

    for (let i = 0; i < moves.length; i++) {
        if (excluded[i]) continue;
        const m = moves[i];
        switch (m.type) {
            case MOVE_ATTACK: {
                let sum = 0;
                for (const c of m.cards) sum += cardScore(c, power);
                rankedInsert(atk, atkK, 12, i, -m.cards.length * 10000 + sum);
                break;
            }
            case MOVE_COVER: {
                let prod = 1;
                for (const c of m.cards) prod *= cardScore(c, power);
                rankedInsert(cov, covK, 10, i, prod - m.cards.length * 0.5);
                break;
            }
            case MOVE_PASS: {
                let sum = 0;
                for (const c of m.cards) sum += cardScore(c, power);
                rankedInsert(pas, pasK, 3, i, sum);
                break;
            }
            case MOVE_GOOD:   goodIdx = i; break;
            case MOVE_PICKUP: pickupIdx = i; break;
        }
    }
    const out: number[] = [];
    for (const i of atk) if (out.length < MAX_CANDS) out.push(i);
    for (const i of cov) if (out.length < MAX_CANDS) out.push(i);
    for (const i of pas) if (out.length < MAX_CANDS) out.push(i);
    if (goodIdx >= 0 && out.length < MAX_CANDS) out.push(goodIdx);
    if (pickupIdx >= 0 && out.length < MAX_CANDS) out.push(pickupIdx);
    return out;
};

// ---------- main MC -----------------------------------------------------------

export interface CorditeParams {
    // Worlds per decision by player count: [W1, W2, W3].
    worldsFor: (numPlayers: number) => [number, number, number];
    // Hard wall-clock budget per decision (ms). Sampling stops gracefully
    // when exceeded; the best candidate so far is returned.
    maxMillis: number;
}

// v2.3 (TS) budget — see the "TS speed-up & budget" note at the top of the
// solver section. After the TS GC-elimination work (pooled rollout trials,
// allocation-free fast chooser, cheaper TT fingerprint) freed ~15-20% per-world
// cost, the world budget is raised ~3x and maxMillis to 2000. Measured offline
// (4-core parallel arena, 80 games/pc, seeded) vs handwritten this lifts win%
// pc2 82.5->92.5, pc4 32.5->41.3 with single-core p99 latency ~0.7-0.9s and
// max ~1.0s (well under the 2s budget). The per-decision maxMillis cap still
// bounds latency, so complex positions degrade gracefully instead of exceeding
// 2s. ~3x is near the identical-world saturation knee; the rest of the freed
// budget is spent on WIDER candidate survival (see the pruning keep-counts in
// corditeChoose), not just more identical worlds.
export const CORDITE_PARAMS: CorditeParams = {
    worldsFor: (n) => n <= 2 ? [96, 168, 168] : n <= 4 ? [84, 168, 168] : [120, 240, n <= 6 ? 168 : 144],
    maxMillis: 2000,
};

// cordite_max — a notch more worlds again, still 2s-bounded, for the strongest
// offline tier.
export const CORDITE_MAX_PARAMS: CorditeParams = {
    worldsFor: (n) => n <= 2 ? [120, 240, 168] : n <= 4 ? [120, 240, 168] : [120, 240, 168],
    maxMillis: 2000,
};

// Choose among `moves` (sim representation of the server's legal moves).
// Returns the index into `moves`.
export const corditeChoose = (pv: PublicView, moves: SimMove[],
        params: CorditeParams): number => {
    if (moves.length === 0) return -1;
    if (moves.length === 1) return 0;
    const t0 = Date.now();

    const B = buildBelief(pv);

    const forcedLoss: boolean[] = new Array(moves.length).fill(false);
    if (!noSolve()) {
        const solved = tryEndgameSolve(pv, B, moves, forcedLoss);
        if (solved >= 0) return solved;
    }

    let cand = pickCandidates(pv.powerSuit, moves, forcedLoss);
    if (cand.length === 0) {
        forcedLoss.fill(false);
        cand = pickCandidates(pv.powerSuit, moves, forcedLoss);
    }
    if (cand.length === 0) return 0;
    if (cand.length === 1) return cand[0];

    const wmul = worldMul();
    let [W1, W2, W3] = params.worldsFor(pv.numPlayers);
    if (wmul !== 1) {
        W1 = Math.max(1, Math.round(W1 * wmul));
        W2 = Math.max(1, Math.round(W2 * wmul));
        W3 = Math.max(1, Math.round(W3 * wmul));
    }

    const base = mix(Math.imul(pv.logs.length, 2654435761) >>> 0,
        ((pv.deckCount << 8) ^ pv.discardLen ^ (pv.myIdx << 20)) >>> 0);

    const score: number[] = new Array(cand.length).fill(0);
    const nsim: number[] = new Array(cand.length).fill(0);
    const alive: boolean[] = new Array(cand.length).fill(true);

    // Single reusable trial buffer for the whole decision (the rollout consumes
    // it fully each iteration, so one is enough — no concurrent live trials).
    const trialScratch = acquireSim(pv.numPlayers);

    let outOfTime = false;
    for (let stage = 0; stage < 3 && !outOfTime; stage++) {
        const wLo = stage === 0 ? 0 : stage === 1 ? W1 : W1 + W2;
        const wHi = stage === 0 ? W1 : stage === 1 ? W1 + W2 : W1 + W2 + W3;
        for (let w = wLo; w < wHi; w++) {
            if (Date.now() - t0 > params.maxMillis) { outOfTime = true; break; }
            const wseed = mix(base, Math.imul(w + 1, 0x85EBCA77) >>> 0);
            const useVoids = (w & 3) !== 3;
            const useFloors = (w & 1) === 0;
            const world = sampleWorld(pv, B, wseed, useVoids, useFloors);
            const simRng = mix(wseed, 0x51AB1E5);
            for (let ci = 0; ci < cand.length; ci++) {
                if (!alive[ci]) continue;
                // Pooled trial: one reused SimGame copied from `world`, mutated
                // to completion by simulate(), then returned to the pool. This
                // is the dominant per-(world x candidate) allocation; pooling it
                // is the bulk of the GC-elimination win.
                const trial = cloneSimInto(trialScratch, world);
                rngSet(simRng);   // identical stream for every candidate (CRN)
                if (!applyMove(trial, pv.myIdx, moves[cand[ci]])) {
                    score[ci] += pv.numPlayers;
                    nsim[ci]++;
                    continue;
                }
                let fp = simulate(trial, pv.myIdx, 600);
                if (fp === 0) fp = pv.numPlayers;
                score[ci] += fp;
                nsim[ci]++;
            }
        }
        if (stage < 2) {
            let nAlive = 0;
            for (const a of alive) if (a) nAlive++;
            // Wider survival than the original max(3, n/3) -> 2. With the larger
            // post-speedup world budget the extra worlds are better spent
            // refining MORE surviving candidates (avoids pruning a strong move
            // on noisy stage-0 estimates) than piling identical worlds onto the
            // top 2 — which saturates. keep ~half after stage 0, 3 after stage 1.
            const keep = stage === 0 ? Math.max(4, Math.ceil(cand.length / 2)) : 3;
            if (keep >= nAlive) continue;
            for (let dropped = nAlive - keep; dropped > 0; dropped--) {
                let worst = -1, worstV = -Infinity;
                for (let i = 0; i < cand.length; i++) {
                    if (!alive[i]) continue;
                    const v = score[i] / (nsim[i] || 1);
                    // >= : drop the LAST tied candidate — candidates are
                    // ranked cheapest-first, and dropping first-tied burns
                    // trumps (the blackpowder tie-break bug; see CORDITE.md).
                    if (v >= worstV) { worstV = v; worst = i; }
                }
                if (worst < 0) break;
                alive[worst] = false;
            }
        }
    }

    releaseSim(trialScratch);

    let best = -1, bestV = Infinity;
    for (let i = 0; i < cand.length; i++) {
        if (!alive[i] || nsim[i] === 0) continue;
        const v = score[i] / nsim[i];
        if (v < bestV) { bestV = v; best = i; }
    }
    return best >= 0 ? cand[best] : 0;
};
