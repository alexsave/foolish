import { Game } from '../core/types.ts';
import { kernelLegalMoves } from '../../../../sdk/ts/wasm/engine.ts';
import { STRAT, wasmChooseMove, wasmChooseMoveDirect } from '../../../../sdk/ts/wasm/bots.ts';
import { BotStrategy, LegalMove } from '../core/bot_interfaces.ts';

// Re-export interfaces for backwards compatibility
export type { BotStrategy, LegalMove };

// Every algorithmic bot runs inside the C kernel (cnitro/src/*_strategy.c
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
// The roster lives in the C kernel (cnitro/src/bot_roster.c): key -> brain +
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

export const BOT_STRATEGIES: Map<string, BotStrategy> = new Map<string, BotStrategy>([
    ['random', new WasmBotStrategy('random', STRAT.random)],
    ['handwritten', new WasmBotStrategy('handwritten', STRAT.handwritten)],
    ['simple_heuristic', new WasmBotStrategy('simple_heuristic', STRAT.simple_heuristic)],
    // logs: espresso's discard memory reads LOG_DISCARD; cordite and the other
    // belief bots build their belief from the full public log.
    ['espresso', new WasmBotStrategy('espresso', STRAT.espresso, { logs: true })],
    // Ladder rungs Medium/Hard (Durak Bot Ordnance Chart). firecracker is
    // robusta's public-info MC with an espresso rollout; blackpowder is the
    // belief-constrained MC with an exact endgame solver. Both rebuild card
    // memory from the public session log, so logs: true hydrates game.belief_logs
    // before they choose (strategyUsesLogs).
    ['firecracker', new WasmBotStrategy('firecracker', STRAT.firecracker, { logs: true })],
    ['blackpowder', new WasmBotStrategy('blackpowder', STRAT.blackpowder, { logs: true })],
    // cordite's CD_BUDGET/CD_RACE and octogen's OG_TRUMP_KEEP now come from
    // the roster (cnitro/src/bot_roster.c), which documents them.
    ['cordite', new WasmBotStrategy('cordite', STRAT.cordite, { logs: true })],
    ['octogen', new WasmBotStrategy('octogen', STRAT.octogen, { logs: true })],
]);

// Lets a harness register a strategy that is not in the table above. The
// offlinefun research harnesses use it, so it STAYS (the `console`/`gpt` I/O
// adapters that motivated it are gone — C_CORE_CONSOLIDATION.md A7).
export const registerBotStrategy = (strategyKey: string, strategy: BotStrategy) => {
    BOT_STRATEGIES.set(strategyKey, strategy);
}

// Get strategy by key
export function getBotStrategy(strategyKey: string): BotStrategy {
    const strategy = BOT_STRATEGIES.get(strategyKey);
    if (!strategy) {
        // Fall back to random strategy if unknown
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
// The enumeration lives in the C kernel (cnitro/src/legal.c
// calculate_legal_moves), compiled to WASM, preserving the exact move
// ordering of the old TS enumerator (verified move-for-move by the
// differential parity harness). One deliberate change: the kernel caps the
// list at 65,536 moves (in enumeration order) where the TS version could
// blow up into millions of combinatorial cover combos and exhaust memory.
export function calculateLegalMoves(game: Game, botPlayerId: string): LegalMove[] {
    return kernelLegalMoves(game, botPlayerId) as LegalMove[];
}
