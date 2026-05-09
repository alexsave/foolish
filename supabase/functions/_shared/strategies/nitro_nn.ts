// Tiny transformer policy for nitro. Pure TypeScript, no ML deps.
//
// The token vocabulary is ~56 — small enough that a 1-layer, 1-head
// transformer is plenty of capacity. Architecture:
//
//   token-ids → [embed + pos] → SelfAttention → FFN → CLS-pool → ProjAction
//
// We tokenize the perfect-info game state as a sequence:
//   [CLS] [ROLE] [DECK_BUCKET]
//   [SEC_HAND] hand-cards
//   [SEC_OPP] opp-cards
//   [SEC_DISCARD] discarded-cards
//   [SEC_TABLE] battle-tokens (attack [BATTLE_COVER] defense | attack-uncovered) [BATTLE_NEXT] ...
//   [SEC_PROGRESS] in-progress-cards
//
// Suits are rotated so trump is always rotSuit=0 — the model sees a
// canonical view independent of the actual trump suit.
//
// The 42-action output: 40 cards (also rotated) + PICKUP + STOP. A multi-
// card move is built by repeatedly asking for one card or STOP.

import { Card, Game, PrivatePlayer, PLAYER_STATUS } from '../types.ts';

// ---------- Token vocabulary ----------

export const NUM_CARDS = 40;            // 4 suits × 10 values (5..14)
export const ACTION_PICKUP = 40;
export const ACTION_STOP = 41;
export const NUM_ACTIONS = 42;

// Special tokens come first; cards follow.
export const TOK_PAD = 0;
export const TOK_CLS = 1;
export const TOK_ROLE_ATK = 2;
export const TOK_ROLE_DEF = 3;
export const TOK_ROLE_FIRST = 4;
export const TOK_DECK_FULL = 5;
export const TOK_DECK_MED = 6;
export const TOK_DECK_LOW = 7;
export const TOK_DECK_EMPTY = 8;
export const TOK_SEC_HAND = 9;
export const TOK_SEC_OPP = 10;
export const TOK_SEC_DISCARD = 11;
export const TOK_SEC_TABLE = 12;
export const TOK_SEC_PROGRESS = 13;
export const TOK_BATTLE_COVER = 14;
export const TOK_BATTLE_NEXT = 15;
// Move-history tokens (prepended to the state portion).
export const TOK_SEC_HISTORY = 16;
export const TOK_PLAYER_SELF = 17;
export const TOK_PLAYER_OPP = 18;
export const TOK_MOVE_ATTACK = 19;
export const TOK_MOVE_COVER = 20;
export const TOK_MOVE_PASS = 21;
export const TOK_MOVE_PICKUP = 22;
export const TOK_MOVE_GOOD = 23;
export const TOK_MOVE_DRAW = 24;
export const TOK_MOVE_DISCARD = 25;
export const TOK_COVER_TARGET = 26;     // separator: cover_card [TARGET] attack_card
export const TOK_CARD_BASE = 32;        // leave a small buffer for future special tokens
export const VOCAB_SIZE = TOK_CARD_BASE + NUM_CARDS; // 72

export const MAX_SEQ_LEN = 192;
// Cap how much history we replay — cheaper inference, and the deeper past
// is summarized by the discard pile / current table state anyway.
export const MAX_HISTORY_EVENTS = 32;

const cardTokenId = (suit: number, value: number, trumpSuit: number): number => {
    const rotSuit = (suit - trumpSuit + 4) % 4;
    const v = Math.max(0, Math.min(9, value - 5));
    return TOK_CARD_BASE + rotSuit * 10 + v;
};

const cardActionId = (suit: number, value: number, trumpSuit: number): number => {
    const rotSuit = (suit - trumpSuit + 4) % 4;
    const v = Math.max(0, Math.min(9, value - 5));
    return rotSuit * 10 + v;
};

export const actionIdToCard = (id: number, trumpSuit: number): { suit: number; value: number } => {
    const rotSuit = Math.floor(id / 10);
    const value = 5 + (id % 10);
    const suit = (rotSuit + trumpSuit) % 4;
    return { suit, value };
};

// ---------- Tokenization ----------

export interface InProgress {
    role: 'attack' | 'cover' | 'pass' | 'idle';
    cardsChosen: Card[];
}

export interface TokenizedState {
    tokens: number[];   // length up to MAX_SEQ_LEN
    clsIdx: number;     // position 0 always
}

export function tokenize(
    game: Game,
    botPlayerId: string,
    inProgress: InProgress,
): TokenizedState {
    const trump = game.power_suit;
    const me = game.players.find(p => p.player_id === botPlayerId)!;
    const opp = game.players.find(p => p.player_id !== botPlayerId && p.status === PLAYER_STATUS.IN);
    const meIdx = game.players.findIndex(p => p.player_id === botPlayerId);
    const isDefender = meIdx === game.defender;
    const isFirstAttack = game.table_battles.length === 0;

    const tokens: number[] = [TOK_CLS];

    // Move history. We stream `game.logs` into a compact token sequence. The
    // discard pile already summarizes long-past events, so we only keep the
    // most recent MAX_HISTORY_EVENTS move events. Each event:
    //   <player_token> <move_type_token> [card-token, ...]
    // For COVER we additionally include the attack card with a TARGET
    // separator so the model can pair cover with what it covered.
    const moveLogs = game.logs.filter(l =>
        l.log_type === 'attack' || l.log_type === 'cover' || l.log_type === 'pass'
        || l.log_type === 'pickup' || l.log_type === 'good'
        || l.log_type === 'discard' || l.log_type === 'draw',
    );
    const recent = moveLogs.slice(Math.max(0, moveLogs.length - MAX_HISTORY_EVENTS));
    if (recent.length > 0) {
        tokens.push(TOK_SEC_HISTORY);
        for (const log of recent) {
            // Player token (only for player-attributed events).
            if (log.player_id) {
                tokens.push(log.player_id === botPlayerId ? TOK_PLAYER_SELF : TOK_PLAYER_OPP);
            }
            switch (log.log_type) {
                case 'attack': tokens.push(TOK_MOVE_ATTACK); break;
                case 'cover': tokens.push(TOK_MOVE_COVER); break;
                case 'pass': tokens.push(TOK_MOVE_PASS); break;
                case 'pickup': tokens.push(TOK_MOVE_PICKUP); break;
                case 'good': tokens.push(TOK_MOVE_GOOD); break;
                case 'discard': tokens.push(TOK_MOVE_DISCARD); break;
                case 'draw': tokens.push(TOK_MOVE_DRAW); break;
                default: continue;
            }
            for (const pair of log.card_pairs) {
                if (pair.primary && pair.primary.suit >= 0) {
                    tokens.push(cardTokenId(pair.primary.suit, pair.primary.value, trump));
                }
                if (pair.target && pair.target.suit >= 0) {
                    tokens.push(TOK_COVER_TARGET);
                    tokens.push(cardTokenId(pair.target.suit, pair.target.value, trump));
                }
            }
            if (tokens.length >= MAX_SEQ_LEN - 30) break; // leave room for state
        }
    }

    if (isFirstAttack && !isDefender) tokens.push(TOK_ROLE_FIRST);
    else if (isDefender) tokens.push(TOK_ROLE_DEF);
    else tokens.push(TOK_ROLE_ATK);

    const deckLeft = game.deck.length + (game.flipped ? 1 : 0);
    if (deckLeft >= 18) tokens.push(TOK_DECK_FULL);
    else if (deckLeft >= 8) tokens.push(TOK_DECK_MED);
    else if (deckLeft >= 1) tokens.push(TOK_DECK_LOW);
    else tokens.push(TOK_DECK_EMPTY);

    // Hand minus already-chosen cards.
    const chosenSet = new Set<string>();
    for (const c of inProgress.cardsChosen) chosenSet.add(`${c.suit}-${c.value}`);
    const liveHand = me.hand.filter(c => !chosenSet.has(`${c.suit}-${c.value}`));
    const sortByRank = (a: Card, b: Card) => {
        const ar = (a.suit - trump + 4) % 4;
        const br = (b.suit - trump + 4) % 4;
        if (ar !== br) return ar - br;
        return a.value - b.value;
    };
    tokens.push(TOK_SEC_HAND);
    for (const c of liveHand.slice().sort(sortByRank)) {
        tokens.push(cardTokenId(c.suit, c.value, trump));
    }
    if (opp) {
        tokens.push(TOK_SEC_OPP);
        for (const c of opp.hand.slice().sort(sortByRank)) {
            tokens.push(cardTokenId(c.suit, c.value, trump));
        }
    }

    // Discard memory (from logs).
    const seen = new Set<string>();
    for (const log of game.logs) {
        if (log.log_type === 'discard') {
            for (const pair of log.card_pairs) {
                if (pair.primary) seen.add(`${pair.primary.suit}-${pair.primary.value}`);
            }
        }
    }
    if (seen.size > 0) {
        tokens.push(TOK_SEC_DISCARD);
        const seenCards: Card[] = [];
        for (const k of seen) {
            const [s, v] = k.split('-').map(n => parseInt(n, 10));
            seenCards.push({ suit: s, value: v });
        }
        for (const c of seenCards.sort(sortByRank)) {
            tokens.push(cardTokenId(c.suit, c.value, trump));
        }
    }

    // Table battles in order.
    if (game.table_battles.length > 0) {
        tokens.push(TOK_SEC_TABLE);
        for (let i = 0; i < game.table_battles.length; i++) {
            if (i > 0) tokens.push(TOK_BATTLE_NEXT);
            const b = game.table_battles[i];
            tokens.push(cardTokenId(b.attack.suit, b.attack.value, trump));
            if (b.defense) {
                tokens.push(TOK_BATTLE_COVER);
                tokens.push(cardTokenId(b.defense.suit, b.defense.value, trump));
            }
        }
    }

    // In-progress move (cards already added this turn).
    if (inProgress.cardsChosen.length > 0) {
        tokens.push(TOK_SEC_PROGRESS);
        for (const c of inProgress.cardsChosen.sort(sortByRank)) {
            tokens.push(cardTokenId(c.suit, c.value, trump));
        }
    }

    if (tokens.length > MAX_SEQ_LEN) {
        tokens.length = MAX_SEQ_LEN;
    }
    return { tokens, clsIdx: 0 };
}

// ---------- Transformer ----------

export const D_MODEL = 32;
export const FF_DIM = 64;          // 2× d_model — small but two layers gives expressivity
export const N_LAYERS = 2;         // small stack
// Single-head attention (multi-head left to a future iteration; this is small enough).

export interface LayerParams {
    Wq: Float32Array; Wk: Float32Array; Wv: Float32Array;  // [D_MODEL][D_MODEL] flat
    Wo: Float32Array;                                       // attention output proj
    ln1g: Float32Array; ln1b: Float32Array;                 // pre-attention LayerNorm
    Wff1: Float32Array; bff1: Float32Array;                 // FFN W1: [FF_DIM][D_MODEL]
    Wff2: Float32Array; bff2: Float32Array;                 // FFN W2: [D_MODEL][FF_DIM]
    ln2g: Float32Array; ln2b: Float32Array;                 // pre-FFN LayerNorm
}

export interface NNParams {
    embed: Float32Array;          // [VOCAB_SIZE][D_MODEL] flat
    posEmbed: Float32Array;       // [MAX_SEQ_LEN][D_MODEL] flat (learned)
    layers: LayerParams[];
    lnFg: Float32Array; lnFb: Float32Array;  // final LayerNorm
    Wout: Float32Array;           // [NUM_ACTIONS][D_MODEL]
    bout: Float32Array;           // [NUM_ACTIONS]
}

const heInit = (rng: () => number, rows: number, cols: number): Float32Array => {
    const a = new Float32Array(rows * cols);
    const std = Math.sqrt(2 / cols);
    for (let i = 0; i < a.length; i++) a[i] = rng() * std;
    return a;
};

const xavierInit = (rng: () => number, rows: number, cols: number): Float32Array => {
    const a = new Float32Array(rows * cols);
    const std = Math.sqrt(1 / cols);
    for (let i = 0; i < a.length; i++) a[i] = rng() * std;
    return a;
};

export function makeRandomParams(seed = 1): NNParams {
    let s = seed >>> 0 || 1;
    const rng = () => {
        s = (s * 1664525 + 1013904223) >>> 0;
        return (s / 4294967296) * 2 - 1;
    };
    const layers: LayerParams[] = [];
    for (let li = 0; li < N_LAYERS; li++) {
        const Lg1 = new Float32Array(D_MODEL); Lg1.fill(1);
        const Lg2 = new Float32Array(D_MODEL); Lg2.fill(1);
        layers.push({
            Wq: xavierInit(rng, D_MODEL, D_MODEL),
            Wk: xavierInit(rng, D_MODEL, D_MODEL),
            Wv: xavierInit(rng, D_MODEL, D_MODEL),
            Wo: xavierInit(rng, D_MODEL, D_MODEL),
            ln1g: Lg1, ln1b: new Float32Array(D_MODEL),
            Wff1: heInit(rng, FF_DIM, D_MODEL), bff1: new Float32Array(FF_DIM),
            Wff2: heInit(rng, D_MODEL, FF_DIM), bff2: new Float32Array(D_MODEL),
            ln2g: Lg2, ln2b: new Float32Array(D_MODEL),
        });
    }
    const lnFg = new Float32Array(D_MODEL); lnFg.fill(1);
    return {
        embed: xavierInit(rng, VOCAB_SIZE, D_MODEL),
        posEmbed: xavierInit(rng, MAX_SEQ_LEN, D_MODEL),
        layers,
        lnFg, lnFb: new Float32Array(D_MODEL),
        Wout: xavierInit(rng, NUM_ACTIONS, D_MODEL),
        bout: new Float32Array(NUM_ACTIONS),
    };
}

// ---------- Forward ----------

interface LayerCache {
    xIn: Float32Array;       // [L][D_MODEL] flat input to the layer
    xLn1: Float32Array;      // post LN1
    ln1Mean: Float32Array; ln1Var: Float32Array; // [L]
    Q: Float32Array; K: Float32Array; V: Float32Array; // [L][D_MODEL]
    scores: Float32Array;    // [L][L]
    attn: Float32Array;      // [L][L] softmax
    attnOut: Float32Array;   // [L][D_MODEL] = attn · V
    proj: Float32Array;      // [L][D_MODEL] = attnOut · Wo
    afterAttn: Float32Array; // residual: xIn + proj
    xLn2: Float32Array;      // post LN2
    ln2Mean: Float32Array; ln2Var: Float32Array;
    ff1pre: Float32Array;    // [L][FF_DIM] linear
    ff1: Float32Array;       // ReLU
    ff2: Float32Array;       // [L][D_MODEL]
    out: Float32Array;       // afterAttn + ff2
}

export interface ForwardCache {
    tokens: number[];
    L: number;
    embedded: Float32Array;   // [L][D_MODEL] embed + pos
    layers: LayerCache[];
    finalLnIn: Float32Array;  // last layer out, sliced to CLS-only
    finalLnMean: number; finalLnVar: number;
    cls: Float32Array;        // [D_MODEL] post-LN
    logits: Float32Array;     // [NUM_ACTIONS]
}

const layerNorm = (x: Float32Array, off: number, g: Float32Array, b: Float32Array, dim: number, eps = 1e-5):
    { out: Float32Array; mean: number; varv: number } => {
    let mean = 0;
    for (let i = 0; i < dim; i++) mean += x[off + i];
    mean /= dim;
    let varv = 0;
    for (let i = 0; i < dim; i++) {
        const d = x[off + i] - mean;
        varv += d * d;
    }
    varv /= dim;
    const inv = 1 / Math.sqrt(varv + eps);
    const out = new Float32Array(dim);
    for (let i = 0; i < dim; i++) {
        out[i] = (x[off + i] - mean) * inv * g[i] + b[i];
    }
    return { out, mean, varv };
};

export function forward(p: NNParams, tokens: number[]): ForwardCache {
    const L = tokens.length;
    const embedded = new Float32Array(L * D_MODEL);
    for (let i = 0; i < L; i++) {
        const tok = tokens[i];
        for (let d = 0; d < D_MODEL; d++) {
            embedded[i * D_MODEL + d] = p.embed[tok * D_MODEL + d] + p.posEmbed[i * D_MODEL + d];
        }
    }

    const layerCaches: LayerCache[] = [];
    let cur = embedded;
    for (let li = 0; li < N_LAYERS; li++) {
        const lp = p.layers[li];
        // Pre-attention LayerNorm (per token).
        const xLn1 = new Float32Array(L * D_MODEL);
        const ln1Mean = new Float32Array(L);
        const ln1Var = new Float32Array(L);
        for (let i = 0; i < L; i++) {
            const r = layerNorm(cur, i * D_MODEL, lp.ln1g, lp.ln1b, D_MODEL);
            ln1Mean[i] = r.mean; ln1Var[i] = r.varv;
            for (let d = 0; d < D_MODEL; d++) xLn1[i * D_MODEL + d] = r.out[d];
        }
        // Q, K, V.
        const Q = new Float32Array(L * D_MODEL);
        const K = new Float32Array(L * D_MODEL);
        const V = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L; i++) {
            for (let d = 0; d < D_MODEL; d++) {
                let q = 0, k = 0, v = 0;
                for (let dd = 0; dd < D_MODEL; dd++) {
                    const x = xLn1[i * D_MODEL + dd];
                    q += lp.Wq[d * D_MODEL + dd] * x;
                    k += lp.Wk[d * D_MODEL + dd] * x;
                    v += lp.Wv[d * D_MODEL + dd] * x;
                }
                Q[i * D_MODEL + d] = q;
                K[i * D_MODEL + d] = k;
                V[i * D_MODEL + d] = v;
            }
        }
        // Attention: scores[i][j] = Q[i] · K[j] / sqrt(d).
        const scale = 1 / Math.sqrt(D_MODEL);
        const scores = new Float32Array(L * L);
        for (let i = 0; i < L; i++) {
            for (let j = 0; j < L; j++) {
                let s = 0;
                for (let d = 0; d < D_MODEL; d++) {
                    s += Q[i * D_MODEL + d] * K[j * D_MODEL + d];
                }
                scores[i * L + j] = s * scale;
            }
        }
        // Softmax row-wise.
        const attn = new Float32Array(L * L);
        for (let i = 0; i < L; i++) {
            let max = -Infinity;
            for (let j = 0; j < L; j++) {
                if (scores[i * L + j] > max) max = scores[i * L + j];
            }
            let sum = 0;
            for (let j = 0; j < L; j++) {
                const e = Math.exp(scores[i * L + j] - max);
                attn[i * L + j] = e;
                sum += e;
            }
            const inv = 1 / sum;
            for (let j = 0; j < L; j++) attn[i * L + j] *= inv;
        }
        // attnOut = attn · V, then proj = attnOut · Wo.
        const attnOut = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L; i++) {
            for (let d = 0; d < D_MODEL; d++) {
                let s = 0;
                for (let j = 0; j < L; j++) {
                    s += attn[i * L + j] * V[j * D_MODEL + d];
                }
                attnOut[i * D_MODEL + d] = s;
            }
        }
        const proj = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L; i++) {
            for (let d = 0; d < D_MODEL; d++) {
                let s = 0;
                for (let dd = 0; dd < D_MODEL; dd++) {
                    s += lp.Wo[d * D_MODEL + dd] * attnOut[i * D_MODEL + dd];
                }
                proj[i * D_MODEL + d] = s;
            }
        }
        // Residual.
        const afterAttn = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L * D_MODEL; i++) afterAttn[i] = cur[i] + proj[i];

        // Pre-FFN LayerNorm.
        const xLn2 = new Float32Array(L * D_MODEL);
        const ln2Mean = new Float32Array(L);
        const ln2Var = new Float32Array(L);
        for (let i = 0; i < L; i++) {
            const r = layerNorm(afterAttn, i * D_MODEL, lp.ln2g, lp.ln2b, D_MODEL);
            ln2Mean[i] = r.mean; ln2Var[i] = r.varv;
            for (let d = 0; d < D_MODEL; d++) xLn2[i * D_MODEL + d] = r.out[d];
        }
        // FFN.
        const ff1pre = new Float32Array(L * FF_DIM);
        const ff1 = new Float32Array(L * FF_DIM);
        for (let i = 0; i < L; i++) {
            for (let h = 0; h < FF_DIM; h++) {
                let s = lp.bff1[h];
                for (let d = 0; d < D_MODEL; d++) {
                    s += lp.Wff1[h * D_MODEL + d] * xLn2[i * D_MODEL + d];
                }
                ff1pre[i * FF_DIM + h] = s;
                ff1[i * FF_DIM + h] = s > 0 ? s : 0;
            }
        }
        const ff2 = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L; i++) {
            for (let d = 0; d < D_MODEL; d++) {
                let s = lp.bff2[d];
                for (let h = 0; h < FF_DIM; h++) {
                    s += lp.Wff2[d * FF_DIM + h] * ff1[i * FF_DIM + h];
                }
                ff2[i * D_MODEL + d] = s;
            }
        }
        // Residual on top of afterAttn.
        const out = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L * D_MODEL; i++) out[i] = afterAttn[i] + ff2[i];

        layerCaches.push({
            xIn: cur, xLn1, ln1Mean, ln1Var, Q, K, V,
            scores, attn, attnOut, proj, afterAttn,
            xLn2, ln2Mean, ln2Var, ff1pre, ff1, ff2, out,
        });
        cur = out;
    }

    // Final LN on CLS only.
    const finalLnIn = cur.slice(0, D_MODEL);
    const r = layerNorm(finalLnIn, 0, p.lnFg, p.lnFb, D_MODEL);
    const cls = r.out;

    const logits = new Float32Array(NUM_ACTIONS);
    for (let a = 0; a < NUM_ACTIONS; a++) {
        let s = p.bout[a];
        for (let d = 0; d < D_MODEL; d++) {
            s += p.Wout[a * D_MODEL + d] * cls[d];
        }
        logits[a] = s;
    }

    return {
        tokens, L,
        embedded, layers: layerCaches,
        finalLnIn, finalLnMean: r.mean, finalLnVar: r.varv,
        cls, logits,
    };
}

// ---------- Softmax + masking ----------

export function softmaxMasked(logits: Float32Array, legal: boolean[]): Float32Array {
    let max = -Infinity;
    for (let i = 0; i < logits.length; i++) {
        if (legal[i] && logits[i] > max) max = logits[i];
    }
    const out = new Float32Array(logits.length);
    let sum = 0;
    for (let i = 0; i < logits.length; i++) {
        if (!legal[i]) { out[i] = 0; continue; }
        const e = Math.exp(logits[i] - max);
        out[i] = e;
        sum += e;
    }
    if (sum > 0) for (let i = 0; i < out.length; i++) out[i] /= sum;
    return out;
}

// ---------- Backprop ----------

export interface NNGrads {
    embed: Float32Array;
    posEmbed: Float32Array;
    layers: Array<{
        Wq: Float32Array; Wk: Float32Array; Wv: Float32Array;
        Wo: Float32Array;
        ln1g: Float32Array; ln1b: Float32Array;
        Wff1: Float32Array; bff1: Float32Array;
        Wff2: Float32Array; bff2: Float32Array;
        ln2g: Float32Array; ln2b: Float32Array;
    }>;
    lnFg: Float32Array; lnFb: Float32Array;
    Wout: Float32Array; bout: Float32Array;
}

export function makeZeroGrads(): NNGrads {
    const layers = [];
    for (let li = 0; li < N_LAYERS; li++) {
        layers.push({
            Wq: new Float32Array(D_MODEL * D_MODEL),
            Wk: new Float32Array(D_MODEL * D_MODEL),
            Wv: new Float32Array(D_MODEL * D_MODEL),
            Wo: new Float32Array(D_MODEL * D_MODEL),
            ln1g: new Float32Array(D_MODEL), ln1b: new Float32Array(D_MODEL),
            Wff1: new Float32Array(FF_DIM * D_MODEL), bff1: new Float32Array(FF_DIM),
            Wff2: new Float32Array(D_MODEL * FF_DIM), bff2: new Float32Array(D_MODEL),
            ln2g: new Float32Array(D_MODEL), ln2b: new Float32Array(D_MODEL),
        });
    }
    return {
        embed: new Float32Array(VOCAB_SIZE * D_MODEL),
        posEmbed: new Float32Array(MAX_SEQ_LEN * D_MODEL),
        layers,
        lnFg: new Float32Array(D_MODEL), lnFb: new Float32Array(D_MODEL),
        Wout: new Float32Array(NUM_ACTIONS * D_MODEL),
        bout: new Float32Array(NUM_ACTIONS),
    };
}

const EPS = 1e-5;

// dx: gradient w.r.t. layer-norm output (length dim). Returns gradient w.r.t.
// the input plus accumulates into g/b grads.
const layerNormBackward = (
    xIn: Float32Array, off: number, mean: number, varv: number,
    g: Float32Array, dxOut: Float32Array,
    dg: Float32Array, db: Float32Array,
    dim: number,
): Float32Array => {
    const inv = 1 / Math.sqrt(varv + EPS);
    const xhat = new Float32Array(dim);
    for (let i = 0; i < dim; i++) xhat[i] = (xIn[off + i] - mean) * inv;
    // Accumulate dg, db.
    for (let i = 0; i < dim; i++) {
        dg[i] += dxOut[i] * xhat[i];
        db[i] += dxOut[i];
    }
    // dxhat = dxOut * g.
    const dxhat = new Float32Array(dim);
    for (let i = 0; i < dim; i++) dxhat[i] = dxOut[i] * g[i];
    // dx = (1/N) * inv * (N*dxhat - sum(dxhat) - xhat * sum(dxhat * xhat))
    let sumDxhat = 0;
    let sumDxhatXhat = 0;
    for (let i = 0; i < dim; i++) {
        sumDxhat += dxhat[i];
        sumDxhatXhat += dxhat[i] * xhat[i];
    }
    const dx = new Float32Array(dim);
    for (let i = 0; i < dim; i++) {
        dx[i] = (1 / dim) * inv * (dim * dxhat[i] - sumDxhat - xhat[i] * sumDxhatXhat);
    }
    return dx;
};

export function accumulateGrads(
    p: NNParams,
    cache: ForwardCache,
    legal: boolean[],
    target: number,
    g: NNGrads,
): number {
    const probs = softmaxMasked(cache.logits, legal);
    const L = cache.L;

    // dL/dlogits = probs - one_hot(target)
    const dlogits = new Float32Array(NUM_ACTIONS);
    for (let i = 0; i < NUM_ACTIONS; i++) dlogits[i] = probs[i];
    dlogits[target] -= 1;

    // logits = Wout · cls + bout
    const dCls = new Float32Array(D_MODEL);
    for (let a = 0; a < NUM_ACTIONS; a++) {
        const dl = dlogits[a];
        if (dl === 0) continue;
        const off = a * D_MODEL;
        for (let d = 0; d < D_MODEL; d++) {
            g.Wout[off + d] += dl * cache.cls[d];
            dCls[d] += p.Wout[off + d] * dl;
        }
        g.bout[a] += dl;
    }

    // Back through final LayerNorm on CLS.
    const dFinalLnIn = layerNormBackward(
        cache.finalLnIn, 0, cache.finalLnMean, cache.finalLnVar,
        p.lnFg, dCls, g.lnFg, g.lnFb, D_MODEL,
    );

    // dOut for last layer (CLS only — other tokens: gradient 0).
    let dOutNext = new Float32Array(L * D_MODEL);
    for (let d = 0; d < D_MODEL; d++) dOutNext[d] = dFinalLnIn[d];

    for (let li = N_LAYERS - 1; li >= 0; li--) {
        const lp = p.layers[li];
        const lc = cache.layers[li];
        const lg = g.layers[li];

        // out = afterAttn + ff2 → both branches.
        const dAfterAttn = new Float32Array(L * D_MODEL);
        const dFf2 = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L * D_MODEL; i++) {
            dAfterAttn[i] = dOutNext[i];
            dFf2[i] = dOutNext[i];
        }

        // ff2[i][d] = sum_h Wff2[d][h] * ff1[i][h] + bff2[d]
        const dFf1 = new Float32Array(L * FF_DIM);
        for (let i = 0; i < L; i++) {
            for (let d = 0; d < D_MODEL; d++) {
                const dy = dFf2[i * D_MODEL + d];
                for (let h = 0; h < FF_DIM; h++) {
                    lg.Wff2[d * FF_DIM + h] += dy * lc.ff1[i * FF_DIM + h];
                    dFf1[i * FF_DIM + h] += lp.Wff2[d * FF_DIM + h] * dy;
                }
                lg.bff2[d] += dy;
            }
        }
        // ReLU through ff1.
        const dFf1pre = new Float32Array(L * FF_DIM);
        for (let i = 0; i < L * FF_DIM; i++) {
            dFf1pre[i] = lc.ff1pre[i] > 0 ? dFf1[i] : 0;
        }
        // ff1pre[i][h] = sum_d Wff1[h][d] * xLn2[i][d] + bff1[h]
        const dXLn2 = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L; i++) {
            for (let h = 0; h < FF_DIM; h++) {
                const dy = dFf1pre[i * FF_DIM + h];
                if (dy === 0) continue;
                for (let d = 0; d < D_MODEL; d++) {
                    lg.Wff1[h * D_MODEL + d] += dy * lc.xLn2[i * D_MODEL + d];
                    dXLn2[i * D_MODEL + d] += lp.Wff1[h * D_MODEL + d] * dy;
                }
                lg.bff1[h] += dy;
            }
        }
        // Back through pre-FFN LN (per token): dAfterAttn += LN_back(dXLn2)
        for (let i = 0; i < L; i++) {
            const dy = new Float32Array(D_MODEL);
            for (let d = 0; d < D_MODEL; d++) dy[d] = dXLn2[i * D_MODEL + d];
            const dx = layerNormBackward(
                lc.afterAttn, i * D_MODEL, lc.ln2Mean[i], lc.ln2Var[i],
                lp.ln2g, dy, lg.ln2g, lg.ln2b, D_MODEL,
            );
            for (let d = 0; d < D_MODEL; d++) dAfterAttn[i * D_MODEL + d] += dx[d];
        }

        // afterAttn = xIn + proj. dXIn (residual) and dProj.
        const dProj = new Float32Array(L * D_MODEL);
        const dXIn = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L * D_MODEL; i++) {
            dProj[i] = dAfterAttn[i];
            dXIn[i] += dAfterAttn[i];
        }

        // proj = attnOut · Wo  (Wo is [D_MODEL][D_MODEL] flat; proj[i][d] = sum_dd Wo[d][dd] * attnOut[i][dd])
        const dAttnOut = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L; i++) {
            for (let d = 0; d < D_MODEL; d++) {
                const dy = dProj[i * D_MODEL + d];
                for (let dd = 0; dd < D_MODEL; dd++) {
                    lg.Wo[d * D_MODEL + dd] += dy * lc.attnOut[i * D_MODEL + dd];
                    dAttnOut[i * D_MODEL + dd] += lp.Wo[d * D_MODEL + dd] * dy;
                }
            }
        }

        // attnOut[i][d] = sum_j attn[i][j] * V[j][d]
        const dAttn = new Float32Array(L * L);
        const dV = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L; i++) {
            for (let d = 0; d < D_MODEL; d++) {
                const dy = dAttnOut[i * D_MODEL + d];
                if (dy === 0) continue;
                for (let j = 0; j < L; j++) {
                    dAttn[i * L + j] += dy * lc.V[j * D_MODEL + d];
                    dV[j * D_MODEL + d] += lc.attn[i * L + j] * dy;
                }
            }
        }

        // Softmax backward row-wise: dscores[i][k] = attn[i][k] * (dAttn[i][k] - sum_j(attn[i][j] * dAttn[i][j]))
        const dScores = new Float32Array(L * L);
        for (let i = 0; i < L; i++) {
            let dot = 0;
            for (let j = 0; j < L; j++) {
                dot += lc.attn[i * L + j] * dAttn[i * L + j];
            }
            for (let k = 0; k < L; k++) {
                dScores[i * L + k] = lc.attn[i * L + k] * (dAttn[i * L + k] - dot);
            }
        }

        // scores[i][j] = (Q[i]·K[j])/sqrt(d). dQ, dK.
        const scale = 1 / Math.sqrt(D_MODEL);
        const dQ = new Float32Array(L * D_MODEL);
        const dK = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L; i++) {
            for (let j = 0; j < L; j++) {
                const ds = dScores[i * L + j] * scale;
                if (ds === 0) continue;
                for (let d = 0; d < D_MODEL; d++) {
                    dQ[i * D_MODEL + d] += ds * lc.K[j * D_MODEL + d];
                    dK[j * D_MODEL + d] += ds * lc.Q[i * D_MODEL + d];
                }
            }
        }

        // Q[i] = Wq · xLn1[i] (i.e. Q[i][d] = sum_dd Wq[d][dd] * xLn1[i][dd])
        const dXLn1 = new Float32Array(L * D_MODEL);
        for (let i = 0; i < L; i++) {
            for (let d = 0; d < D_MODEL; d++) {
                const dy = dQ[i * D_MODEL + d];
                if (dy !== 0) {
                    for (let dd = 0; dd < D_MODEL; dd++) {
                        lg.Wq[d * D_MODEL + dd] += dy * lc.xLn1[i * D_MODEL + dd];
                        dXLn1[i * D_MODEL + dd] += lp.Wq[d * D_MODEL + dd] * dy;
                    }
                }
                const dyk = dK[i * D_MODEL + d];
                if (dyk !== 0) {
                    for (let dd = 0; dd < D_MODEL; dd++) {
                        lg.Wk[d * D_MODEL + dd] += dyk * lc.xLn1[i * D_MODEL + dd];
                        dXLn1[i * D_MODEL + dd] += lp.Wk[d * D_MODEL + dd] * dyk;
                    }
                }
                const dyv = dV[i * D_MODEL + d];
                if (dyv !== 0) {
                    for (let dd = 0; dd < D_MODEL; dd++) {
                        lg.Wv[d * D_MODEL + dd] += dyv * lc.xLn1[i * D_MODEL + dd];
                        dXLn1[i * D_MODEL + dd] += lp.Wv[d * D_MODEL + dd] * dyv;
                    }
                }
            }
        }

        // Back through pre-attn LN (per token): dXIn += LN_back(dXLn1).
        for (let i = 0; i < L; i++) {
            const dy = new Float32Array(D_MODEL);
            for (let d = 0; d < D_MODEL; d++) dy[d] = dXLn1[i * D_MODEL + d];
            const dx = layerNormBackward(
                lc.xIn, i * D_MODEL, lc.ln1Mean[i], lc.ln1Var[i],
                lp.ln1g, dy, lg.ln1g, lg.ln1b, D_MODEL,
            );
            for (let d = 0; d < D_MODEL; d++) dXIn[i * D_MODEL + d] += dx[d];
        }

        dOutNext = dXIn;
    }

    // dOutNext now contains gradient w.r.t. embedded[i] for each token.
    for (let i = 0; i < L; i++) {
        const tok = cache.tokens[i];
        const tokOff = tok * D_MODEL;
        const posOff = i * D_MODEL;
        for (let d = 0; d < D_MODEL; d++) {
            const dy = dOutNext[i * D_MODEL + d];
            g.embed[tokOff + d] += dy;
            g.posEmbed[posOff + d] += dy;
        }
    }

    return -Math.log(Math.max(1e-9, probs[target]));
}

// Apply gradient updates with global-norm gradient clipping. Without
// clipping a few outlier samples can produce huge gradients that wreck
// already-learned weights — exactly what we saw on resume training (loss
// climbed from 2.43 to 3.5+ over an epoch).
export function applyGrads(p: NNParams, g: NNGrads, lr: number, batchSize: number, clipNorm = 1.0): void {
    const inv = 1 / batchSize;
    // Compute global L2 norm of the (mean) gradient across all parameter
    // tensors.
    let sq = 0;
    const all: Float32Array[] = [
        g.embed, g.posEmbed, g.lnFg, g.lnFb, g.Wout, g.bout,
    ];
    for (const lg of g.layers) {
        all.push(lg.Wq, lg.Wk, lg.Wv, lg.Wo,
            lg.ln1g, lg.ln1b,
            lg.Wff1, lg.bff1, lg.Wff2, lg.bff2,
            lg.ln2g, lg.ln2b);
    }
    for (const a of all) {
        for (let i = 0; i < a.length; i++) {
            const v = a[i] * inv;
            sq += v * v;
        }
    }
    const norm = Math.sqrt(sq);
    const scale = norm > clipNorm ? clipNorm / norm : 1;
    const eff = lr * inv * scale;

    const upd = (a: Float32Array, da: Float32Array) => {
        for (let i = 0; i < a.length; i++) a[i] -= eff * da[i];
        da.fill(0);
    };
    upd(p.embed, g.embed);
    upd(p.posEmbed, g.posEmbed);
    for (let li = 0; li < N_LAYERS; li++) {
        const lp = p.layers[li]; const lg = g.layers[li];
        upd(lp.Wq, lg.Wq); upd(lp.Wk, lg.Wk); upd(lp.Wv, lg.Wv); upd(lp.Wo, lg.Wo);
        upd(lp.ln1g, lg.ln1g); upd(lp.ln1b, lg.ln1b);
        upd(lp.Wff1, lg.Wff1); upd(lp.bff1, lg.bff1);
        upd(lp.Wff2, lg.Wff2); upd(lp.bff2, lg.bff2);
        upd(lp.ln2g, lg.ln2g); upd(lp.ln2b, lg.ln2b);
    }
    upd(p.lnFg, g.lnFg); upd(p.lnFb, g.lnFb);
    upd(p.Wout, g.Wout); upd(p.bout, g.bout);
}

// ---------- (De)serialization ----------

export function serializeParams(p: NNParams): string {
    return JSON.stringify({
        meta: { vocab: VOCAB_SIZE, dModel: D_MODEL, ffDim: FF_DIM, nLayers: N_LAYERS, maxSeq: MAX_SEQ_LEN },
        embed: Array.from(p.embed),
        posEmbed: Array.from(p.posEmbed),
        layers: p.layers.map(l => ({
            Wq: Array.from(l.Wq), Wk: Array.from(l.Wk), Wv: Array.from(l.Wv), Wo: Array.from(l.Wo),
            ln1g: Array.from(l.ln1g), ln1b: Array.from(l.ln1b),
            Wff1: Array.from(l.Wff1), bff1: Array.from(l.bff1),
            Wff2: Array.from(l.Wff2), bff2: Array.from(l.bff2),
            ln2g: Array.from(l.ln2g), ln2b: Array.from(l.ln2b),
        })),
        lnFg: Array.from(p.lnFg), lnFb: Array.from(p.lnFb),
        Wout: Array.from(p.Wout), bout: Array.from(p.bout),
    });
}

export function deserializeParams(json: string): NNParams | null {
    try {
        const j = JSON.parse(json);
        const m = j.meta;
        if (!m || m.vocab !== VOCAB_SIZE || m.dModel !== D_MODEL
            || m.ffDim !== FF_DIM || m.nLayers !== N_LAYERS
            || m.maxSeq !== MAX_SEQ_LEN) return null;
        return {
            embed: new Float32Array(j.embed),
            posEmbed: new Float32Array(j.posEmbed),
            layers: j.layers.map((l: any) => ({
                Wq: new Float32Array(l.Wq), Wk: new Float32Array(l.Wk),
                Wv: new Float32Array(l.Wv), Wo: new Float32Array(l.Wo),
                ln1g: new Float32Array(l.ln1g), ln1b: new Float32Array(l.ln1b),
                Wff1: new Float32Array(l.Wff1), bff1: new Float32Array(l.bff1),
                Wff2: new Float32Array(l.Wff2), bff2: new Float32Array(l.bff2),
                ln2g: new Float32Array(l.ln2g), ln2b: new Float32Array(l.ln2b),
            })),
            lnFg: new Float32Array(j.lnFg), lnFb: new Float32Array(j.lnFb),
            Wout: new Float32Array(j.Wout), bout: new Float32Array(j.bout),
        };
    } catch {
        return null;
    }
}

export { cardActionId };
