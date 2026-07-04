import { Game } from './types.ts';
import { kernelLegalMoves } from './wasm/engine.ts';
import { STRAT, wasmChooseMove, wasmChooseMoveDirect } from './wasm/bots.ts';
import { BotStrategy, LegalMove } from './bot_interfaces.ts';
import { GPTBotStrategy } from './strategies/gpt_strategy.ts';
import { NitroStrategy } from './strategies/nitro_strategy.ts';

// Re-export interfaces for backwards compatibility
export type { BotStrategy, LegalMove };

// Every algorithmic bot runs inside the C kernel (cnitro/src/*_strategy.c
// compiled to bots.wasm) — single source of truth for bot play, same as the
// rules. This adapter marshals the game in, lets the kernel enumerate legal
// moves and choose, and maps the returned index onto the caller's list (the
// orderings are identical: both come from the kernel's enumerator).
//
// The two remaining TS-brained strategies are deliberate exceptions:
//   - gpt/console: I/O-bound adapters (LLM calls, stdin), not game logic.
//   - nitro: the experimental transformer NN (nitro_nn.ts + JSON weights) —
//     a research artifact that plateaued below cordite (see README); porting
//     an NN runtime to freestanding C isn't worth it unless it ever wins.
export class WasmBotStrategy implements BotStrategy {
    readonly name: string;
    private strat: number;
    private env?: Record<string, string>;
    // Only the belief/memory bots read the session log; skipping the log
    // marshal for the rest removes the hottest TS frame of a bot turn.
    private logs: boolean;

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

// Strategy registry. CD_BUDGET selects the kernel cordite's world/pruning
// budget: 'prod' mirrors the deployed v2.4 player-count-aware budget,
// 'max' the larger cordite_max tier (see cnitro/src/cordite_strategy.c).
export const BOT_STRATEGIES: Map<string, BotStrategy> = new Map<string, BotStrategy>([
    ['random', new WasmBotStrategy('random', STRAT.random)],
    ['handwritten', new WasmBotStrategy('handwritten', STRAT.handwritten)],
    ['simple_heuristic', new WasmBotStrategy('simple_heuristic', STRAT.simple_heuristic)],
    ['ultimate_champion', new WasmBotStrategy('ultimate_champion', STRAT.ultimate_champion)],
    ['champion', new WasmBotStrategy('champion', STRAT.champion)],
    ['hacker', new WasmBotStrategy('hacker', STRAT.hacker)],
    // logs: espresso's discard memory reads LOG_DISCARD; cordite/fulminate
    // build their belief from the full public log.
    ['espresso', new WasmBotStrategy('espresso', STRAT.espresso, { logs: true })],
    ['nitro', new NitroStrategy()],
    // CD_RACE stops a deliberation early once the leading candidate is
    // statistically separated (validated strength-neutral at C=75: pc4x800
    // identical, pc2/pc6 within noise; landslide decisions finish in ~50
    // worlds instead of ~900).
    ['cordite', new WasmBotStrategy('cordite', STRAT.cordite, { env: { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' }, logs: true })],
    ['cordite_max', new WasmBotStrategy('cordite_max', STRAT.cordite, { env: { CD_BUDGET: 'max', CD_RACE: '1', CD_RACE_C: '75' }, logs: true })],
    ['fulminate', new WasmBotStrategy('fulminate', STRAT.fulminate, { env: { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' }, logs: true })],
]);

// Lazy-load GPT strategy to avoid requiring API key at module load time
let gptStrategyInstance: GPTBotStrategy | null = null;

// So we can manuall include console or gpt strategy
export const registerBotStrategy = (strategyKey: string, strategy: BotStrategy) => {
    BOT_STRATEGIES.set(strategyKey, strategy);
}

// Get strategy by key
export function getBotStrategy(strategyKey: string): BotStrategy {
    // Handle GPT strategy separately with lazy loading
    if (strategyKey === 'gpt') {
        if (!gptStrategyInstance) {
            try {
                gptStrategyInstance = new GPTBotStrategy();
                BOT_STRATEGIES.set('gpt', gptStrategyInstance);
            } catch (error) {
                console.error('Failed to initialize GPT strategy:', error);
                console.log('Falling back to random strategy');
                return BOT_STRATEGIES.get('random')!;
            }
        }
        return gptStrategyInstance;
    }

    const strategy = BOT_STRATEGIES.get(strategyKey);
    if (!strategy) {
        // Fall back to random strategy if unknown
        return BOT_STRATEGIES.get('random')!;
    }
    return strategy;
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
