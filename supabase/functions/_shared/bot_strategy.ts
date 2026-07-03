import { Game } from './types.ts';
import { kernelLegalMoves } from './wasm/engine.ts';
import { BotStrategy, LegalMove } from './bot_interfaces.ts';
import { RandomBotStrategy } from './strategies/random_strategy.ts';
import { HandwrittenBotStrategy } from './strategies/handwritten_strategy.ts';
import { SimpleHeuristicStrategy } from './strategies/simple_heuristic_strategy.ts';
import { UltimateChampionStrategy } from './strategies/ultimate_champion_strategy.ts';
import { ChampionStrategy } from './strategies/champion_strategy.ts';
import { HackerStrategy } from './strategies/hacker_strategy.ts';
import { GPTBotStrategy } from './strategies/gpt_strategy.ts';
import { EspressoStrategy } from './strategies/espresso_strategy.ts';
import { NitroStrategy } from './strategies/nitro_strategy.ts';
import { CorditeStrategy, CorditeMaxStrategy } from './strategies/cordite_strategy.ts';
import { FulminateStrategy } from './strategies/fulminate_strategy.ts';

// Re-export interfaces for backwards compatibility
export type { BotStrategy, LegalMove };

// Strategy registry
export const BOT_STRATEGIES: Map<string, BotStrategy> = new Map<string, BotStrategy>([
    ['random', new RandomBotStrategy()],
    ['handwritten', new HandwrittenBotStrategy()],
    ['simple_heuristic', new SimpleHeuristicStrategy()],
    ['ultimate_champion', new UltimateChampionStrategy()],
    ['champion', new ChampionStrategy()],
    ['hacker', new HackerStrategy()],
    ['espresso', new EspressoStrategy()],
    ['nitro', new NitroStrategy()],
    ['cordite', new CorditeStrategy()],
    ['cordite_max', new CorditeMaxStrategy()],
    ['fulminate', new FulminateStrategy()],
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
