/* =============================================================================
 * Infinite Oracle — shared types & tuning constants
 * (docs/INFINITE_ORACLE_DESIGN.md). Imported by the worker, the controller,
 * the overlay, and the headless test — one source of truth for every shape.
 * ========================================================================== */

/* --------------------------- the analysis job ---------------------------- */

/** Structure-clone-safe job shipped to every worker (§8.2). The position is the
 *  kernel's bytes, imported by oracle.wasm unchanged: no field of the board is
 *  read or laid out here. */
export interface OracleJob {
    decisionId: string;             // `${code}:${j}:${memoryOn?1:0}`
    seat: number;                   // acting seat
    memoryOn: boolean;
    /** The board the seat decided on, as it saw it: what a masked
     *  wasm_import_state reads (c/src/replay_steps.h replay_steps_board_v6). */
    state: Uint8Array;
    /** The public log before the move, what wasm_import_logs reads
     *  (replay_steps_memory_v6); empty when memory is off. */
    logsWire: Uint8Array;
    powerSuit: number;              // the trump suit, for the dump's card tokens
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
// packed little-endian blob (wasm_og_paths_ptr/len - no JSON on the hot batch
// path). Decoder: pathsBlob.ts. Round-outcome symbols (cd_orc,
// c/src/cordite_sim.h): 1 = we defended and beat the round, 2 = we were
// forced to pick up, 3 = an opponent beat the round, 4 = an opponent picked
// up. A shorter seq than the round count means the playout resolved (game
// over or exact leaf) inside the recorded window.
//
// Mode A ONLY. oracle-mt.wasm carries no trace hooks at all (CD_ORC_TRACE in
// c/src/cordite_sim.h says why), so under Mode B every candidate's `why` is
// undefined and the overlay renders no proof panel.

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
     *  replies, whole-playout marginals). Absent until a sidecar arrives, and
     *  always absent under Mode B. */
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
    /** Decision-static context for the belief display (from the dump record).
     *  Mode A only - Mode B has no JSON record to read it from. */
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
export function oracleCardToken(c: { suit: number; value: number }, trump: number): string {
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

/* -------------------------------- flags ---------------------------------- */

/** The click-to-open "why" proof panel (docs/INFINITE_ORACLE_DESIGN.md §9.7).
 *  OFF in production, and off in a default dev run.
 *
 *  The replay route is a self-contained base32 payload that needs no auth and
 *  no database row, so a client-side panel cannot be metered. The code lands
 *  here so it stops rotting in a branch, not so it ships enabled.
 *
 *  This is the web's first feature flag, and it deliberately reuses the one
 *  gate shape the app already has - a build-time environment variable, like
 *  FOOLISH_CROSS_ORIGIN_ISOLATION in next.config.mjs - rather than inventing a
 *  parallel knob system. NEXT_PUBLIC_ is what makes it readable from the
 *  client: Next inlines it at build time, so with the flag off the whole panel
 *  is statically unreachable. The shipping value below is the default, and the
 *  env var is the documented override, exactly as ios/FoolishApp/PhoneOnly/
 *  Flags.swift derives its debug override from a shipping constant.
 *
 *  Turn it on for development:  NEXT_PUBLIC_FOOLISH_ORACLE_WHY=1 npm run dev
 */
export const ORACLE_WHY_PANEL_SHIPPING = false;

export function oracleWhyPanelEnabled(): boolean {
    const v = process.env.NEXT_PUBLIC_FOOLISH_ORACLE_WHY;
    if (v === '1') return true;
    if (v === '0') return false;
    return ORACLE_WHY_PANEL_SHIPPING;
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

/** Minimum wall time between two published snapshots while a run is in flight.
 *  The fleet posts a batch every few milliseconds and the overlay is a ~6,000
 *  node LED array, so asking for a publish per animation frame asks React and
 *  the compositor to redraw the whole panel 60 times a second to move an error
 *  bar by less than a pixel. Measured over 6 s of deliberation (production
 *  build, replay screen), this interval roughly halves what that costs: the
 *  renderer main thread drops from 1,377 ms busy to 610 ms, the GPU process
 *  from 3,651 ms to 1,839 ms, and Paint from 152 ms to 76 ms.
 *
 *  It does not make the sharpening any coarser - it makes it FINER. Asking for
 *  a frame the machine cannot afford means the panel actually repainted 3.8
 *  times a second, with a median 299 ms between updates; asking at this
 *  interval it repaints 8.4 times a second, with a median gap of 84 ms.
 *
 *  It is a floor, not a schedule: publishes still land on an animation frame,
 *  and a terminal snapshot (converged/exact/forced/error) ignores it entirely. */
export const ORACLE_PUBLISH_MS = 80;

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
