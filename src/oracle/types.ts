/* =============================================================================
 * Infinite Oracle — shared types & tuning constants
 * (docs/INFINITE_ORACLE_DESIGN.md). Imported by the worker, the controller,
 * the overlay, and the headless test — one source of truth for every shape.
 * ========================================================================== */

import { Card } from '@api/core/types.ts';

/* --------------------------- the analysis job ---------------------------- */

/** A Game-shaped object __marshalGame (engine.ts) accepts. Only the fields
 *  marshalGame reads are present; good/elimination are player_id STRINGS
 *  ('seat-N') — numeric seats fail silently in __marshalGame (§8.4). */
export interface OracleGameState {
    id: string;
    status: string;                 // GAME_STATUS.PLAYING
    power_suit: number;
    first_attacker: number;
    defender: number;
    discard_pile_length: number;
    flipped: Card | null;
    good_players: string[];         // 'seat-N'[]
    good_timestamp: number | null;
    deck: Card[];                   // deckCount placeholders (COUNT is meaningful)
    table_battles: { attack: Card; defense: Card | null }[];
    elimination_order: string[];    // 'seat-N'[], ordered
    deterministic_deck: boolean;    // always false — replay has no deal seed
    players: {
        player_id: string;          // 'seat-N'
        status: string;             // PLAYER_STATUS.IN / .OUT
        name: string;
        is_ai: boolean;
        hand_length: number;
        hand: Card[];               // acting seat: real; others: placeholders
        awaiting_attack: boolean;   // inert — always false (§8.4)
    }[];
}

/** Structured-clone-safe job shipped to every worker (§8.2). */
export interface OracleJob {
    decisionId: string;             // `${code}:${j}:${memoryOn?1:0}`
    seat: number;                   // acting seat
    memoryOn: boolean;
    gameBlob: OracleGameState;
    logsWire: Uint8Array;           // pre-encoded kernel log wire (empty if memory off)
    recordedKey: string;            // canonical key of the recorded move (§9.4)
    recordedLabel: string;          // human label of the recorded move
    numPlayers: number;
    deckAlive: boolean;             // step.deckCount > 0 || flipped !== null (tax gate)
    approx: boolean;                // §5.2 null-slot fill happened (should be unreachable)
    eliminations: number;           // # seats already out at this decision (EF floor check)
}

/* ------------------------- the wasm dump records ------------------------- */

export type OracleVerdict = 'none' | 'unknown' | 'illegal' | 'win' | 'loss' | 'draw';

/** One candidate line from og_ex_emit's JSONL dump. */
export interface OracleDumpCandidate {
    type: string;
    label: string;
    cards: string[];
    target?: string[];
    score: number | null;          // mean finish this record, null if nsim==0
    nsim: number;
    alive: number;
    pruned?: number;               // §6.3 verdict-only entry for a pruned move
    forced_loss: number;
    verdict: OracleVerdict;
    verdict_val?: number;
    chosen: number;
}

/** Octogen's belief block, emitted per record (og_ex_emit): cards publicly
 *  PINNED to each seat's hand, the genuinely-unknown pool, per-seat void
 *  constraints (attack cards the seat demonstrably could not beat) and rank
 *  floors. The raw material of the overlay's belief display. */
export interface OracleDumpBelief {
    pinned: string[][];
    pool: string[];
    voids: string[][];
    floor: number[];
}

export interface OracleDumpRecord {
    seat: number;
    deck: number;
    defender: number;
    trump: number;
    belief?: OracleDumpBelief;
    hand?: string[];
    hand_count?: number;
    opp_counts?: number[];
    table?: { attack: string; defense: string | null }[];
    solver: { applied: number; result: string };
    candidates: OracleDumpCandidate[];
    chosen: string;
    overflow?: number;             // §6.3 staging-buffer overflow marker
}

/* ---------------------- MC path sidecar (binary blob) --------------------- */
// Per-candidate playout storylines, shipped NEXT TO the JSON record as a
// packed little-endian blob (wasm_og_paths_ptr/len — no JSON on the hot batch
// path). Decoder: pathsBlob.ts. Round-outcome symbols (cd_orc,
// c/src/cordite_sim.h): 1 = we defended and beat the round, 2 = we were
// forced to pick up, 3 = an opponent beat the round, 4 = an opponent picked
// up. A shorter seq than the round count means the playout resolved (game
// over or exact leaf) inside the recorded window.

export interface OraclePathStat { seq: number[]; n: number; fin: number; }
/** First move by any non-hero seat after the root move: type indexes
 *  MV_ATTACK..MV_GOOD (0..4), card is a 0..51 id or 52 for card-less. */
export interface OracleReplyStat { type: number; card: number; n: number; }
export interface OracleCandAgg {
    n: number;                     // playouts folded
    mepk: number;                  // my pickups per playout
    oppk: number;                  // opponent pickups per playout
    metr: number;                  // my trump cards spent per playout
    opptr: number;                 // opponent trump cards spent per playout
    rnds: number;                  // rounds resolved per playout
}
export interface OracleCandPaths {
    agg: OracleCandAgg;
    replies: OracleReplyStat[];
    paths: OraclePathStat[];
}

/* --------------------- worker <-> controller protocol -------------------- */

export type WorkerToMain =
    | { t: 'ready' }
    | { t: 'batch'; decisionId: string; record: OracleDumpRecord; batchMs: number; gen: number; paths?: ArrayBuffer }
    | { t: 'exact'; decisionId: string; gen: number }
    | { t: 'forced'; decisionId: string; gen: number }
    | { t: 'empty'; decisionId: string; gen: number }
    | { t: 'error'; decisionId: string; gen: number; message: string };

export type MainToWorker =
    | { t: 'init'; bytes: Uint8Array }
    | { t: 'analyze'; job: OracleJob; seedSalt: number; gen: number }
    | { t: 'stop' };

/* --------------------------- merged UI shapes ---------------------------- */

/** A candidate accumulated across every batch, ready for the overlay. */
export interface OracleCandidate {
    key: string;
    type: string;
    label: string;
    cards: string[];
    target?: string[];
    n: number;                     // cumulative nsim
    mean: number | null;           // sum/n, mean finish (lower = better)
    se: number;                    // standard error of the mean (batch-mean stddev / √batches)
    adjusted: number | null;       // mean + trump tax (display ranking)
    verdict: OracleVerdict;
    verdictVal?: number;
    forcedLoss: boolean;
    pruned: boolean;
    chosen: boolean;
    played: boolean;               // matches the recorded move
    /** Merged MC path data for the "why" panel (top storylines, most likely
     *  replies, whole-playout marginals). Absent until a sidecar arrives. */
    why?: OracleCandPaths;
}

export type OracleStatus =
    | 'idle' | 'loading' | 'running' | 'converged' | 'exact' | 'forced' | 'error';

/** The immutable snapshot the controller publishes to the overlay. */
export interface OracleSnapshot {
    decisionId: string;
    status: OracleStatus;
    regime: 'mc' | 'exact';
    candidates: OracleCandidate[];
    totalWorlds: number;
    worldsPerSec: number;
    batches: number;
    elapsedMs: number;
    memoryOn: boolean;
    seat: number;
    recordedKey: string;
    recordedLabel: string;
    recordedPresent: boolean;      // recorded move appeared among candidates
    approx: boolean;
    deckAlive: boolean;
    numPlayers: number;
    /** Decision-static context for the belief display (from the dump record). */
    belief?: {
        pinned: string[][];
        voids: string[][];
        floor: number[];
        poolCount: number;
        hand: string[];
        oppCounts: number[];
        table: { attack: string; defense: string | null }[];
        defender: number;
        trump: number;
    };
    error?: string;
}

/* --------------------- canonical move keys (§9.4) ------------------------ */

// Card grammar of the wasm dump (octogen_strategy.c og_ex_fmt_card): value via
// OG_EX_VAL, suit via "SHCD", trump-starred. Recorded-move tokens must match so
// the recorded move keys to its candidate row.
const OG_EX_VAL = ['?', '2', '3', '4', '5', '6', '7', '8', '9', '10', 'J', 'Q', 'K', 'A'];
export function oracleCardToken(c: Card, trump: number): string {
    const v = c.value >= 1 && c.value <= 13 ? OG_EX_VAL[c.value] : '?';
    const s = c.suit >= 0 && c.suit < 4 ? 'SHCD'[c.suit] : '?';
    return `${v}${s}${c.suit === trump ? '*' : ''}`;
}

/** Order-insensitive canonical key: type | sorted cards | sorted targets.
 *  Mirrors the X-ray normLabel precedent (gen_html.py). */
export function canonicalMoveKey(type: string, cards: string[], target?: string[]): string {
    const c = [...cards].sort().join(',');
    const t = [...(target ?? [])].sort().join(',');
    return `${type}|${c}|${t}`;
}

/* ------------------------------ tuning knobs ----------------------------- */

/** octogen's trump-keep tax (OG_TRUMP_KEEP default 40 milli = 0.040 per trump
 *  card in an attack while the deck is alive). Applied at selection only; the
 *  dumped scores are UNTAXED, so the client re-applies it for display ranking,
 *  exactly as the X-ray pages do (octogen_strategy.c OG_TRUMP_KEEP default;
 *  bot_strategy.ts deployed env; build_data.py). */
export const ORACLE_TRUMP_KEEP = 0.040;

/** Worker fleet size: clamp(cores - 2, 1, 8). */
export const ORACLE_MAX_WORKERS = 8;
export function oracleWorkerCount(): number {
    const cores = (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 4;
    return Math.max(1, Math.min(ORACLE_MAX_WORKERS, cores - 2));
}

/** Batch sizing (§8.6): target ~40 ms/batch; adapt OG_W1 to device speed. */
export const ORACLE_W1_START = 24;
export const ORACLE_W1_MIN = 8;
export const ORACLE_W1_MAX = 192;
export const ORACLE_BATCH_FAST_MS = 25;   // below → double W1
export const ORACLE_BATCH_SLOW_MS = 80;   // above → halve W1

/** Per-move endgame verdict probe budget (per-call getenv). */
export const ORACLE_SOLVE_BUDGET = 2_000_000;

/** Convergence checkpoint (§8.7), computed over candidates with n > 0 ONLY. */
export const ORACLE_CONVERGE_MIN_N = 65_536;
export const ORACLE_CONVERGE_MAX_SE = 0.005;
export const ORACLE_HARD_CAP_MS = 180_000;

/** Minimum "come into focus" duration. On a fast device the estimate can meet
 *  the checkpoint in well under a second; keep the fleet sampling until at least
 *  this long so the deliberation is perceptible and the error bars visibly
 *  shrink (more worlds only sharpen the estimate — never worse). §9.3. */
export const ORACLE_MIN_FOCUS_MS = 3_500;

/** Focus animation: SE at which a row is "fully in focus" (§9.3). */
export const ORACLE_SE0 = 0.25;

/** chess.com-style move classification, on ADJUSTED scores relative to best. */
export const ORACLE_CLASS_THRESHOLDS: { max: number; id: OracleClass }[] = [
    { max: 0.0001, id: 'best' },
    { max: 0.05, id: 'excellent' },
    { max: 0.15, id: 'good' },
    { max: 0.35, id: 'inaccuracy' },
    { max: 0.7, id: 'mistake' },
    { max: Infinity, id: 'blunder' },
];
export type OracleClass =
    | 'best' | 'excellent' | 'good' | 'inaccuracy' | 'mistake' | 'blunder';

export function oracleClassify(delta: number): OracleClass {
    for (const t of ORACLE_CLASS_THRESHOLDS) if (delta < t.max) return t.id;
    return 'blunder';
}
