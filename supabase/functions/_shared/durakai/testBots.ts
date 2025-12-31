#!/usr/bin/env node
/**
 * Unified Bot Tournament Test Runner
 * 
 * Usage:
 *   npx tsx testBots.ts <strategy1> <strategy2> [options]
 *   npx tsx testBots.ts <strategy1>:<count1> <strategy2>:<count2> ... [options]
 * 
 * Examples:
 *   npx tsx testBots.ts handwritten random --games 100
 *   npx tsx testBots.ts handwritten:4 random:4 --games 50
 *   npx tsx testBots.ts log-aware handwritten:3 random:2 --games 100
 * 
 * Available strategies: random, handwritten, simple_heuristic, champion, ultimate_champion, hacker, log-aware
 * 
 * Player counts: 2-8 players supported
 */

import { Game, PLAYER_STATUS } from '../types';
import { BotStrategy } from '../bot_interfaces';
import { BOT_STRATEGIES } from '../bot_strategy';
import { calculateLegalMoves } from '../bot_strategy';
import { setRandomSeed } from '../random_strategy';
import { 
    initializeGame, 
    isTerminal, 
    getFinishingOrder, 
    applyMove,
    getLegalMoves,
    getCurrentPlayer,
    SimpleMove,
    cardToString,
    replenishHands,
    activePlayerCount
} from './gameEngine';

// -------------------------------------------------------------------------
//                          Strategy Registry
// -------------------------------------------------------------------------

// Extend the bot strategies with our custom log-aware strategy
const ALL_STRATEGIES = new Map<string, BotStrategy | { name: string; selectMove: (game: Game, playerIndex: number) => SimpleMove }>();

// Add all strategies from BOT_STRATEGIES
BOT_STRATEGIES.forEach((strategy, key) => {
    ALL_STRATEGIES.set(key, strategy);
});

// -------------------------------------------------------------------------
//                          Game Playing Logic
// -------------------------------------------------------------------------

interface GameResult {
    winner: string | null;
    finishingOrder: number[];
    moveCount: number;
    logCount: number;
}

interface TournamentResult {
    totalGames: number;
    player0Wins: number;
    player1Wins: number;
    draws: number;
    player0WinRate: number;
    player1WinRate: number;
    averageMoves: number;
    averageLogs: number;
}

async function playGame(
    strategies: any[], 
    maxMoves: number = -1, // Auto-calculate based on player count
    debug: boolean = false,
    seed?: string,
    logToFile: boolean = false // New parameter to enable move_analysis.txt generation
): Promise<GameResult> {
    const playerCount = strategies.length;
    if (playerCount < 2 || playerCount > 8) {
        throw new Error('Player count must be between 2 and 8');
    }
    
    // Auto-calculate move limit based on player count
    if (maxMoves === -1) {
        maxMoves = playerCount <= 2 ? 500 : 10000; // Much higher limit for multi-player
    }
    
    // Setup file logging if requested
    let moveAnalysisLog = '';
    const logToAnalysis = (msg: string) => {
        if (logToFile) {
            moveAnalysisLog += msg + '\n';
        }
        if (debug) {
            console.log(msg);
        }
    };
    
    const game = initializeGame(playerCount, seed); // Pass seed to initializeGame
    let moveCount = 0;
    let lastMoveType: string | null = null;
    let lastMoveCount = 0;
    let lastPlayerIndex = game.first_attacker; // Track who just moved
    
    if (debug || logToFile) {
        logToAnalysis(`${'='.repeat(60)}`);
        logToAnalysis(`Starting ${playerCount}-player game:`);
        logToAnalysis(`Flipped card: ${cardToString(game.flipped!)}`);
        logToAnalysis(`Trump suit: ${['♠', '♥', '♦', '♣'][game.power_suit]}`);
        logToAnalysis(`Deck size: ${game.deck.length}`);
        logToAnalysis(`First attacker: P${game.first_attacker}, Defender: P${game.defender}`);
        logToAnalysis(`\nInitial hands:`);
        for (let i = 0; i < playerCount; i++) {
            logToAnalysis(`  P${i} (${strategies[i].name}): ${game.players[i].hand.map(cardToString).join(', ')} (${game.players[i].hand.length} cards)`);
        }
        logToAnalysis('');
    }
    
    while (!isTerminal(game) && moveCount < maxMoves) {
        if (debug && moveCount > 0 && moveCount % 10 === 0) {
            const activePlayers = game.players.filter(p => p.status === PLAYER_STATUS.IN);
            console.log(`[After move ${moveCount}] Active players: ${activePlayers.length}, Deck: ${game.deck.length}, Table: ${game.table_battles.length}`);
        }
        
        // Get current player from game engine
        const currentPlayerIndex = getCurrentPlayer(game);
        
        if (currentPlayerIndex === -1) {
            // Game should be over or there's an error
            console.error(`[BREAK] getCurrentPlayer returned -1 at move ${moveCount}`);
            console.error(`  isTerminal: ${isTerminal(game)}`);
            console.error(`  table: ${game.table_battles.length}, deck: ${game.deck.length}`);
            console.error(`  first_attacker: ${game.first_attacker}, defender: ${game.defender}`);
            console.error(`  Players:`);
            for (let i = 0; i < game.players.length; i++) {
                console.error(`    P${i}: status=${game.players[i].status}, hand=${game.players[i].hand.length}`);
            }
            break;
        }
        
        const currentPlayer = game.players[currentPlayerIndex];
        
        // Additional check: if current player is OUT, something is wrong
        if (currentPlayer.status !== PLAYER_STATUS.IN) {
            console.error(`[BUG] getCurrentPlayer returned OUT player P${currentPlayerIndex} at move ${moveCount}`);
            console.error(`  Player status: ${currentPlayer.status}, hand: ${currentPlayer.hand.length}`);
            console.error(`  table: ${game.table_battles.length}, allCovered: ${game.table_battles.every(b => b.defense !== null)}`);
            console.error(`  All player statuses:`);
            for (let i = 0; i < game.players.length; i++) {
                console.error(`    P${i}: status=${game.players[i].status}`);
            }
            break;
        }
        
        // Safety check for infinite loops
        if (moveCount > 80 && moveCount % 20 === 0 && game.table_battles.length > 0) {
            const allCovered = game.table_battles.every(b => b.defense !== null);
            if (allCovered || moveCount % 500 === 0) {
                console.log(`[WARNING] Potential infinite loop at move ${moveCount}. Table battles: ${game.table_battles.length}, All covered: ${allCovered}`);
                console.log(`  Current player: ${currentPlayerIndex} (${currentPlayer.name}), Defender: ${game.defender}, First attacker: ${game.first_attacker}`);
                console.log(`  good_players: [${game.good_players?.join(', ') || 'none'}]`);
                
                // Check what moves are legal
                const simpleMoves = getLegalMoves(game, currentPlayerIndex);
                console.log(`  Simple moves available: ${simpleMoves.length} (types: ${simpleMoves.map(m => m.type).join(', ')})`);
            }
        }
        
        // Skip if player is OUT (shouldn't happen, but be safe)
        if (currentPlayer.status !== PLAYER_STATUS.IN) {
            if (debug) console.log(`[Move ${moveCount}] Player ${currentPlayerIndex} is OUT, breaking.`);
            break;
        }
        
        const currentStrategy = strategies[currentPlayerIndex];
        
        try {
            // Check if this is the log-aware strategy (uses game engine interface directly)
            if (currentStrategy.name === 'log-aware-handwritten') {
                const simpleMoves = getLegalMoves(game, currentPlayerIndex);
                if (simpleMoves.length === 0) {
                    if (debug) console.log(`[Move ${moveCount}] No legal moves for player ${currentPlayerIndex}. Breaking.`);
                    break;
                }
                
                const move = currentStrategy.selectMove(game, currentPlayerIndex);
                
                if (debug) {
                    console.log(`\nMove ${moveCount + 1}: ${currentStrategy.name} plays ${move.type}${move.card ? ' ' + cardToString(move.card) : ''}`);
                }
                
                applyMove(game, currentPlayerIndex, move);
            } else {
                // Regular BotStrategy - uses LegalMove interface
                const legalMoves = calculateLegalMoves(game, currentPlayer.player_id);
                if (legalMoves.length === 0) {
                    const simpleMoves = getLegalMoves(game, currentPlayerIndex);
                    if (simpleMoves.length === 0) {
                        if (debug) console.log(`[Move ${moveCount}] No legal moves for player ${currentPlayerIndex}. Breaking.`);
                        break;
                    }
                    applyMove(game, currentPlayerIndex, simpleMoves[0]);
                } else {
                    const chosenMove = await currentStrategy.chooseMove(game, currentPlayer.player_id, legalMoves);
                    
                    // Debug: log what was chosen when we suspect a loop
                    if (moveCount >= 95 && moveCount <= 105 && game.table_battles.length > 0) {
                        const allCovered = game.table_battles.every(b => b.defense !== null);
                        console.log(`  [Move ${moveCount}] P${currentPlayerIndex} chose: ${chosenMove.type}, allCovered: ${allCovered}, hasPlayerSaidGood: ${game.good_players?.includes(currentPlayer.player_id) || false}`);
                    }
                    
                        // Convert to simple move
                        let simpleMove: SimpleMove;
                        if (chosenMove.type === 'attack') {
                            // For attacks with cards, pass ALL cards (multi-card attack)
                            if (chosenMove.cards && chosenMove.cards.length > 0) {
                                simpleMove = { type: 'attack', cards: chosenMove.cards };
                            } else if (chosenMove.done_attacking_this_round && game.table_battles.length > 0) {
                                // Only convert to 'good' if we're in the middle of a round with battles on table
                                // and the bot explicitly chose to be done (with no cards specified)
                                const allCovered = game.table_battles.every(b => b.defense !== null);
                                if (allCovered) {
                                    simpleMove = { type: 'good' };
                                } else {
                                    // Shouldn't happen, but fallback
                                    const fallback = getLegalMoves(game, currentPlayerIndex);
                                    simpleMove = fallback[0];
                                }
                            } else {
                                const fallback = getLegalMoves(game, currentPlayerIndex);
                                simpleMove = fallback[0];
                            }
                        } else if (chosenMove.type === 'cover' && chosenMove.cards && chosenMove.cards.length > 0) {
                            simpleMove = { type: 'defend', cards: chosenMove.cards };
                        } else if (chosenMove.type === 'pass' && chosenMove.cards && chosenMove.cards.length > 0) {
                            simpleMove = { type: 'pass', card: chosenMove.cards[0] }; // Pass is still single-card
                        } else if (chosenMove.type === 'pickup') {
                            simpleMove = { type: 'pickup' };
                        } else if (chosenMove.type === 'good') {
                            simpleMove = { type: 'good' };
                        } else if (chosenMove.type === 'wait') {
                            simpleMove = { type: 'wait' };
                        } else {
                            const fallback = getLegalMoves(game, currentPlayerIndex);
                            simpleMove = fallback[0];
                        }
                    
                    // Store old state before applying move
                    const oldDefender = game.defender;
                    const oldFirstAttacker = game.first_attacker;
                    const oldPlayerStatuses = game.players.map(p => p.status);
                    const oldHandSizes = game.players.map(p => p.hand.length);
                    const oldTableSize = game.table_battles.length;
                    
                    // Apply the move FIRST
                    applyMove(game, currentPlayerIndex, simpleMove);
                    
                    if (debug || logToFile) {
                        // Format move summary - handle both single and multi-card moves
                        let moveSummary: string;
                        if (simpleMove.cards && simpleMove.cards.length > 1) {
                            // Multi-card move
                            const cardStr = simpleMove.cards.map(cardToString).join(', ');
                            moveSummary = `${simpleMove.type} [${cardStr}]`;
                        } else if (simpleMove.cards && simpleMove.cards.length === 1) {
                            moveSummary = `${simpleMove.type} ${cardToString(simpleMove.cards[0])}`;
                        } else if (simpleMove.card) {
                            moveSummary = `${simpleMove.type} ${cardToString(simpleMove.card)}`;
                        } else {
                            moveSummary = simpleMove.type;
                        }
                        
                        logToAnalysis(`\nMove ${moveCount + 1}: P${currentPlayerIndex} (${currentStrategy.name}) plays ${moveSummary}`);
                        
                        // Check for draws (replenish happened)
                        for (let i = 0; i < game.players.length; i++) {
                            const cardsDrawn = game.players[i].hand.length - oldHandSizes[i];
                            if (cardsDrawn > 0 && simpleMove.type !== 'pickup' && simpleMove.type !== 'pass') {
                                const drawnCards = game.players[i].hand.slice(-cardsDrawn);
                                logToAnalysis(`  → P${i} drew ${cardsDrawn} card(s): ${drawnCards.map(cardToString).join(', ')}`);
                            }
                        }
                        
                        // Check for players going OUT
                        for (let i = 0; i < game.players.length; i++) {
                            if (oldPlayerStatuses[i] === PLAYER_STATUS.IN && game.players[i].status === PLAYER_STATUS.OUT) {
                                logToAnalysis(`  → P${i} is OUT!`);
                            }
                        }
                        
                        // Log round changes - table cleared means new round (pickup, good, or discard)
                        if (oldTableSize > 0 && game.table_battles.length === 0 && moveCount > 0) {
                            // Check if it was a discard (successful defense) or pickup
                            if (simpleMove.type === 'good' || (simpleMove.type === 'defend' && oldTableSize > 0)) {
                                logToAnalysis(`  → Table discarded (${oldTableSize} battle(s))`);
                            }
                            
                            // Check if roles changed
                            if (oldDefender !== game.defender || oldFirstAttacker !== game.first_attacker) {
                                logToAnalysis(`  → Roles changed: First attacker=P${game.first_attacker}, Defender=P${game.defender}`);
                            }
                            logToAnalysis(`  → Deck size: ${game.deck.length}`);
                            
                            // Show all hands at the start of a new round
                            logToAnalysis(`  → Round start - Hands:`);
                            for (let i = 0; i < game.players.length; i++) {
                                const status = game.players[i].status === PLAYER_STATUS.OUT ? ' [OUT]' : '';
                                logToAnalysis(`     P${i}${status}: ${game.players[i].hand.map(cardToString).join(', ')} (${game.players[i].hand.length} cards)`);
                            }
                        }
                    }
                    
                    // Track move types for loop detection
                    if (simpleMove.type === lastMoveType) {
                        lastMoveCount++;
                    } else {
                        lastMoveType = simpleMove.type;
                        lastMoveCount = 1;
                    }
                }
            }
            
            // Track this player as the last one who moved
            lastPlayerIndex = currentPlayerIndex;
            
            moveCount++;
            
            // Debug: detect stuck states
            if (moveCount % 100 === 0) {
                const allCovered = game.table_battles.every(b => b.defense !== null);
                const activePlayers = game.players.filter(p => p.status === PLAYER_STATUS.IN).length;
                if (moveCount % 1000 === 0) {
                    console.log(`[Move ${moveCount}] Active: ${activePlayers}, Table: ${game.table_battles.length}, AllCovered: ${allCovered}, Logs: ${game.logs.length}`);
                }
            }
        } catch (error) {
            console.error(`Error from ${currentStrategy.name}:`, error);
            break;
        }
    }
    
    const finishingOrder = getFinishingOrder(game);
    const winner = game.players.find(p => p.hand.length === 0)?.player_id || null;
    
    // Debug: check for draw conditions
    if (finishingOrder.length === 0 && moveCount < maxMoves) {
        const activeCount = game.players.filter(p => p.status === PLAYER_STATUS.IN).length;
        console.error(`[DRAW DEBUG] Game ended as draw at ${moveCount} moves`);
        console.error(`  isTerminal: ${isTerminal(game)}, activeCount: ${activeCount}`);
        console.error(`  Player statuses:`);
        for (let i = 0; i < game.players.length; i++) {
            console.error(`    P${i}: status=${game.players[i].status}, hand=${game.players[i].hand.length}`);
        }
    }
    
    if (debug || logToFile) {
        logToAnalysis(`\n${'='.repeat(60)}`);
        logToAnalysis('Game Over!');
        if (finishingOrder.length > 0) {
            logToAnalysis(`Winner: Player ${finishingOrder[0]} (${strategies[finishingOrder[0]].name})`);
        } else {
            logToAnalysis('Draw - no winner');
        }
        logToAnalysis(`Total moves: ${moveCount}`);
        logToAnalysis(`Total logs: ${game.logs.length}`);
        logToAnalysis(`${'='.repeat(60)}\n`);
    }
    
    // Write move_analysis.txt if logging to file
    if (logToFile && moveAnalysisLog) {
        try {
            const { writeFileSync } = await import('fs');
            writeFileSync('move_analysis.txt', moveAnalysisLog);
            console.log(`✅ Wrote move_analysis.txt (${moveAnalysisLog.split('\n').length} lines)`);
        } catch (e) {
            console.error('Failed to write move_analysis.txt:', e);
        }
    }
    
    return {
        winner,
        finishingOrder,
        moveCount,
        logCount: game.logs.length
    };
}

async function playTournament(
    strategies: any[], 
    numGames: number, 
    verbose: boolean = true,
    seed?: string
): Promise<TournamentResult> {
    const playerCount = strategies.length;
    const winsByStrategy = new Map<string, number>();
    const winsByPlayer = new Array(playerCount).fill(0);
    
    // Initialize win counters
    strategies.forEach(s => winsByStrategy.set(s.name, 0));
    
    let draws = 0;
    let totalMoves = 0, totalLogs = 0;
    let longestGame = { moves: 0, gameNum: -1, seed: '' };
    
    if (verbose) {
        console.log(`\n${'='.repeat(60)}`);
        console.log(`Playing ${numGames} games with ${playerCount} players:`);
        strategies.forEach((s, i) => {
            console.log(`  Player ${i}: ${s.name}`);
        });
        console.log(`${'='.repeat(60)}\n`);
    }
    
    for (let i = 0; i < numGames; i++) {
        const gameSeed = seed ? `${seed}_game${i}` : undefined;
        const result = await playGame(strategies, -1, false, gameSeed);
        totalMoves += result.moveCount;
        totalLogs += result.logCount;
        
        // Track longest game
        if (result.moveCount > longestGame.moves) {
            longestGame = { moves: result.moveCount, gameNum: i, seed: gameSeed || 'none' };
        }
        
        if (result.finishingOrder.length > 0) {
            const winner = result.finishingOrder[0];
            winsByPlayer[winner]++;
            const strategyName = strategies[winner].name;
            winsByStrategy.set(strategyName, (winsByStrategy.get(strategyName) || 0) + 1);
        } else {
            draws++;
        }
        
        if (verbose && (i + 1) % 10 === 0) {
            const winRates = strategies.map((s, idx) => {
                const wins = winsByPlayer[idx];
                const rate = wins / (i + 1);
                return `P${idx}(${s.name}): ${(rate * 100).toFixed(1)}%`;
            }).join(' | ');
            console.log(`Game ${(i + 1).toString().padStart(3)} | ${winRates}`);
        }
    }
    
    const averageMoves = totalMoves / numGames;
    const averageLogs = totalLogs / numGames;
    
    // If there's a game with >1000 moves, replay it with full logging
    if (longestGame.moves > 1000) {
        console.log(`\n⚠️  Longest game had ${longestGame.moves} moves (game #${longestGame.gameNum})! Replaying with full logging...\n`);
        
        try {
            const { writeFileSync } = await import('fs');
            
            // Capture console output
            const originalLog = console.log;
            let capturedOutput = '';
            console.log = (...args: any[]) => {
                const line = args.join(' ') + '\n';
                capturedOutput += line;
                originalLog(...args);
            };
            
            await playGame(strategies, -1, true, longestGame.seed);
            
            // Restore console.log
            console.log = originalLog;
            
            // Write full log
            writeFileSync('templog.txt', capturedOutput);
            
            // Extract move lines for analysis
            const moveLines = capturedOutput.split('\n')
                .filter(line => line.startsWith('Move ') || line.includes('→ Roles changed'))
                .join('\n');
            writeFileSync('move_analysis.txt', moveLines);
            
            console.log(`\n✅ Logged to templog.txt and move_analysis.txt`);
        } catch (e) {
            console.error('Failed to write log files:', e);
        }
    }
    
    if (verbose) {
        console.log(`\n${'='.repeat(60)}`);
        console.log('Tournament Results');
        console.log(`${'='.repeat(60)}`);
        console.log(`Total games: ${numGames}`);
        console.log('\nWins by player:');
        strategies.forEach((s, idx) => {
            const wins = winsByPlayer[idx];
            const rate = wins / numGames;
            console.log(`  Player ${idx} (${s.name}): ${wins} wins (${(rate * 100).toFixed(1)}%)`);
        });
        console.log('\nWins by strategy:');
        winsByStrategy.forEach((wins, strategyName) => {
            const rate = wins / numGames;
            console.log(`  ${strategyName}: ${wins} wins (${(rate * 100).toFixed(1)}%)`);
        });
        console.log(`\nDraws: ${draws}`);
        console.log(`Average moves per game: ${averageMoves.toFixed(1)}`);
        console.log(`Average logs per game: ${averageLogs.toFixed(1)}`);
        console.log(`${'='.repeat(60)}\n`);
    }
    
    return {
        totalGames: numGames,
        player0Wins: winsByPlayer[0],
        player1Wins: winsByPlayer[1],
        draws,
        player0WinRate: winsByPlayer[0] / numGames,
        player1WinRate: winsByPlayer[1] / numGames,
        averageMoves,
        averageLogs
    };
}

// -------------------------------------------------------------------------
//                          CLI Interface
// -------------------------------------------------------------------------

function printUsage() {
    console.log('Durak Bot Tournament Test Runner');
    console.log('=================================\n');
    console.log('Usage:');
    console.log('  npx tsx testBots.ts <strategy1> <strategy2> [options]');
    console.log('  npx tsx testBots.ts <strategy1>:<count1> <strategy2>:<count2> ... [options]\n');
    console.log('Options:');
    console.log('  --games <n>     Number of games to play (default: 100)');
    console.log('  --seed <n>      Random seed for reproducible tests');
    console.log('  --quiet         Minimal output');
    console.log('  --debug         Show detailed move-by-move output (first game only)');
    console.log('  --list          List all available strategies\n');
    console.log('Available strategies:');
    ALL_STRATEGIES.forEach((_, key) => {
        console.log(`  - ${key}`);
    });
    console.log('\nExamples:');
    console.log('  2-player: npx tsx testBots.ts handwritten random');
    console.log('  8-player: npx tsx testBots.ts handwritten:4 random:4 --games 50');
    console.log('  Seeded:   npx tsx testBots.ts handwritten:4 random:4 --seed 12345');
    console.log('  Debug:    npx tsx testBots.ts handwritten:2 random:2 --debug');
    console.log('\nNote: Multi-player games (3-8 players) take much longer than 2-player games.');
}

async function main() {
    const args = process.argv.slice(2);
    let seedValue: string | undefined;
    
    // Check for seed argument
    const seedIndex = args.indexOf('--seed');
    if (seedIndex !== -1 && seedIndex + 1 < args.length) {
        seedValue = args[seedIndex + 1];
        console.log(`Using random seed: ${seedValue}`);
        // Remove seed args
        args.splice(seedIndex, 2);
    }
    
    // Handle --list flag
    if (args.includes('--list')) {
        console.log('Available strategies:');
        ALL_STRATEGIES.forEach((strategy, key) => {
            console.log(`  ${key.padEnd(20)} - ${strategy.name}`);
        });
        return;
    }
    
    // Parse arguments
    if (args.length < 1 || args.includes('--help') || args.includes('-h')) {
        printUsage();
        return;
    }
    
    // Parse strategy specifications (strategy:count or just strategy)
    const strategies: any[] = [];
    let argIdx = 0;
    
    while (argIdx < args.length && !args[argIdx].startsWith('--')) {
        const spec = args[argIdx];
        const parts = spec.split(':');
        const strategyName = parts[0];
        const count = parts.length > 1 ? parseInt(parts[1]) : 1;
        
        const strategy = ALL_STRATEGIES.get(strategyName);
        if (!strategy) {
            console.error(`Error: Unknown strategy "${strategyName}"`);
            console.error('Use --list to see available strategies');
            process.exit(1);
        }
        
        // Add this strategy 'count' times
        for (let i = 0; i < count; i++) {
            strategies.push(strategy);
        }
        
        argIdx++;
    }
    
    if (strategies.length < 2) {
        console.error('Error: Need at least 2 players');
        printUsage();
        process.exit(1);
    }
    
    if (strategies.length > 8) {
        console.error('Error: Maximum 8 players allowed');
        process.exit(1);
    }
    
    let numGames = 100;
    let verbose = true;
    let debug = false;
    
    for (let i = argIdx; i < args.length; i++) {
        if (args[i] === '--games' && i + 1 < args.length) {
            numGames = parseInt(args[i + 1]);
            i++;
        } else if (args[i] === '--quiet') {
            verbose = false;
        } else if (args[i] === '--debug') {
            debug = true;
        }
    }
    
    console.log('Durak Bot Tournament');
    console.log('====================\n');
    
    // Run debug game if requested
    if (debug) {
        console.log('🐛 DEBUG MODE: Playing one detailed game...');
        await playGame(strategies, 500, true, seedValue, true); // Enable both debug and file logging
        console.log('\nContinuing with tournament...\n');
    }
    
    // Run tournament
    await playTournament(strategies, numGames, verbose, seedValue);
}

// Run if executed directly
if (import.meta.url === `file://${process.argv[1]}`) {
    main().catch(console.error);
}

export { playGame, playTournament, ALL_STRATEGIES };

