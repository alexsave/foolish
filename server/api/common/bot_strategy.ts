import { Game } from '@api/core/types.ts';
import { kernelLegalMoves } from '@sdk/ts/wasm/engine.ts';
import { kernelBotRoster, wasmChooseMove, wasmChooseMoveDirect } from '@sdk/ts/wasm/bots.ts';
import { BotStrategy, LegalMove } from '@api/core/bot_interfaces.ts';

// Re-export interfaces for backwards compatibility
export type { BotStrategy, LegalMove };

// Every algorithmic bot runs inside the C kernel (c/src/*_strategy.c
// compiled to bots.wasm) — single source of truth for bot play, same as the
// rules. This adapter marshals the game in, lets the kernel enumerate legal
// moves and choose, and maps the returned index onto the caller's list (the
// orderings are identical: both come from the kernel's enumerator).
//
// There are no TS-brained strategies left: the two exceptions — `gpt` (LLM
// calls) and `console` (stdin), I/O adapters rather than game logic — were
// never seeded in production and are deleted (C_CORE_CONSOLIDATION.md A7).
export class WasmBotStrategy implements BotStrategy {
    readonly name: string;
    private strat: number;
    private env?: Record<string, string>;
    // Only the belief/memory bots read the session log; skipping the log
    // marshal for the rest removes the hottest TS frame of a bot turn. Public
    // so the server bot loop knows whether to hydrate game.belief_logs before
    // this strategy chooses (strategyUsesLogs).
    readonly logs: boolean;

    constructor(name: string, strat: number, opts: { env?: Record<string, string>; logs?: boolean } = {}) {
        this.name = name;
        this.strat = strat;
        this.env = opts.env;
        this.logs = opts.logs ?? false;
    }

    chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        try {
            const idx = wasmChooseMove(game, botPlayerId, this.strat, { env: this.env, logs: this.logs });
            if (idx >= 0 && idx < legalMoves.length) {
                return Promise.resolve(legalMoves[idx]);
            }
        } catch (error) {
            console.error(`[${this.name}] kernel chooseMove failed, falling back to first legal move:`, error);
        }
        return Promise.resolve(legalMoves[0]);
    }

    // Fast path for the bot loop: the kernel enumerates AND picks in one
    // call and only the chosen move crosses back — no TS-side move-list
    // materialization. null = no legal moves. Falls back to the list path
    // in processBotAction on error.
    chooseMoveDirect(game: Game, botPlayerId: string): LegalMove | null {
        return wasmChooseMoveDirect(game, botPlayerId, this.strat, { env: this.env, logs: this.logs });
    }
}

// Strategy registry.
//
// The roster lives in the C kernel (c/src/bot_roster.c): key -> brain +
// knobs + logs flag, one table shared with the phone and every future client
// (docs/C_CORE_CONSOLIDATION.md F1/A1). This map used to restate that table's
// knobs as `env` blocks, kept "deliberately identical" by hand and by a parity
// test. They are gone: wasm_choose_move now resolves brain AND knobs through
// bot_roster_choose, exactly as bot_drive's cycle has since A2, so the tuning
// that makes cordite `cordite` travels with the kernel to every host instead of
// being re-typed per client. A bot's identity is kernel data, like the rules.
//
// What is left here is what the kernel genuinely cannot know: which key the DB
// hands us, and whether to hydrate the session log first. `env` survives as a
// per-call override (bot_knobs.h) for harnesses that need a fast budget.

// Built FROM the kernel's roster (bot_roster.c), not restated beside it. Every
// row this build can actually run becomes a dispatchable strategy, carrying the
// kernel's own brain id and logs flag - so a bot added, retuned or reordered in
// C reaches the server without a second edit here. The map stays mutable
// because registerBotStrategy below lets a harness add one the kernel has no
// entry for.
//
// Lazily: touching it instantiates bots.wasm, and a lobby-only cold start must
// not pay that.
let REGISTRY: Map<string, BotStrategy> | null = null;

function registry(): Map<string, BotStrategy> {
    if (REGISTRY) return REGISTRY;
    REGISTRY = new Map<string, BotStrategy>();
    for (const e of kernelBotRoster()) {
        // The SEEDED set - the bots seed.sql actually creates on the site, and
        // the only keys a production seat can carry. Not `linked`: robusta and
        // gunpowder are offline-only rungs that this build can run but the site
        // never deals, and e2e/bot_roster_parity.test.ts holds that they must
        // not acquire a registration by accident. espresso is the same case and
        // is not even linked here (FOOLISH_SEEDED_BOTS_ONLY), so registering it
        // bought a seat that would have played the first legal move under
        // espresso's name.
        if (!e.seeded) continue;
        REGISTRY.set(e.key, new WasmBotStrategy(e.key, e.strat, { logs: e.usesLogs }));
    }
    return REGISTRY;
}

/** @deprecated Prefer resolveBotStrategy / getBotStrategy; this materializes
 *  the kernel roster as a Map for the few callers that iterate it. */
export const BOT_STRATEGIES = { get: (k: string) => registry().get(k), set: (k: string, v: BotStrategy) => registry().set(k, v), keys: () => registry().keys() };

// Lets a harness register a strategy that is not in the table above. The
// offlinefun research harnesses use it, so it STAYS (the `console`/`gpt` I/O
// adapters that motivated it are gone — C_CORE_CONSOLIDATION.md A7).
export const registerBotStrategy = (strategyKey: string, strategy: BotStrategy) => {
    BOT_STRATEGIES.set(strategyKey, strategy);
}

// Strict lookup: the registered strategy for this key, or null. No fallback.
//
// The kernel is strict about exactly this (wasm_choose_move: an unknown or
// unlinked strat returns -1, "never a silent fallback to random"), but the TS
// layer resolves the key BEFORE the kernel ever sees it, so that guarantee
// cannot fire for a key the registry does not know. Anything that MEASURES a
// named bot — a benchmark, a memory gate, an arena — must resolve through this
// and refuse an unknown key, or it is measuring `random` under another name and
// reporting it green (issue #111: the edge-memory CI gate did exactly that,
// unnoticed, for every one of the bots it named).
export function resolveBotStrategy(strategyKey: string): BotStrategy | null {
    return BOT_STRATEGIES.get(strategyKey) ?? null;
}

// The keys the registry can actually dispatch. For an error message that names
// the alternatives rather than making the reader grep for them.
export const botStrategyKeys = (): string[] => [...BOT_STRATEGIES.keys()];

// Warned-about keys, so a bot seat with a bad key does not log once per turn
// for the life of the game.
const warnedUnknownKeys = new Set<string>();

// Get strategy by key, for the PLAY path: a seat whose key the registry does
// not know still has to take its turn, so this keeps the random fallback. What
// it no longer does is keep it QUIET — a typo'd strategy_key used to become a
// random bot with no signal anywhere, on the leaderboard included.
export function getBotStrategy(strategyKey: string): BotStrategy {
    const strategy = BOT_STRATEGIES.get(strategyKey);
    if (!strategy) {
        if (!warnedUnknownKeys.has(strategyKey)) {
            warnedUnknownKeys.add(strategyKey);
            console.warn(`[bot_strategy] unknown strategy_key ${JSON.stringify(strategyKey)} — `
                + `falling back to 'random'. It will PLAY like random and score like random. `
                + `Known keys: ${botStrategyKeys().join(', ')}`);
        }
        return BOT_STRATEGIES.get('random')!;
    }
    return strategy;
}

// Does this bot's strategy read the session log to build a belief over hidden
// cards? The server bot loop uses this to hydrate game.belief_logs only when a
// belief bot is about to act — the beliefless strategies keep the fast path.
export function strategyUsesLogs(strategyKey: string): boolean {
    const s = BOT_STRATEGIES.get(strategyKey);
    return s instanceof WasmBotStrategy && s.logs;
}

// Calculate all legal moves for a bot given current game state.
// The enumeration lives in the C kernel (c/src/legal.c
// calculate_legal_moves), compiled to WASM, preserving the exact move
// ordering of the old TS enumerator (verified move-for-move by the
// differential parity harness). One deliberate change: the kernel caps the
// list at 65,536 moves (in enumeration order) where the TS version could
// blow up into millions of combinatorial cover combos and exhaust memory.
export function calculateLegalMoves(game: Game, botPlayerId: string): LegalMove[] {
    return kernelLegalMoves(game, botPlayerId) as LegalMove[];
}
