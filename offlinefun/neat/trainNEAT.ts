#!/usr/bin/env tsx
/**
 * NEAT trainer (handwritten-opponent fitness).
 *
 * Fitness = sum of win-rates across player counts 2..8 where the table is:
 *   1 NEAT vs (N-1) handwritten bots
 *
 * "Win" per user = NEAT finishes in the top half:
 *  - 2p: 1st
 *  - 3p: 1st
 *  - 4/5p: 1st or 2nd
 *  - 6/7p: 1st/2nd/3rd
 *  - 8p: 1st/2nd/3rd/4th
 *
 * Timeouts/engine anomalies are treated as losses (no draws).
 */

import {
    initializeGame,
    applyMove,
    isTerminal,
    getCurrentPlayer,
    getFinishingOrder,
    getLegalMoves
} from '../../supabase/functions/_shared/durakai/gameEngine';
import { FeatureExtractor } from '../../supabase/functions/_shared/durakai/featureExtractor';
import { NEATBotStrategy } from './neatBotStrategy';
import { HandwrittenBotStrategy } from '../../supabase/functions/_shared/handwritten_strategy';
import { calculateLegalMoves } from '../../supabase/functions/_shared/bot_strategy';
import { BotStrategy, LegalMove } from '../../supabase/functions/_shared/bot_interfaces';
import { writeFileSync, readFileSync, existsSync, mkdirSync } from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

// @ts-ignore
import { architect, Network, methods } from 'neataptic';

const POPULATION_SIZE = 50;
const GENERATIONS = 1000;

const FITNESS_PLAYER_COUNTS = [2, 3, 4, 5, 6, 7, 8] as const;
const FITNESS_GAMES_PER_SIZE = Number.parseInt(process.env.FITNESS_GAMES_PER_SIZE ?? '10', 20);

const MOVE_LIMIT_2P = 500;
const MOVE_LIMIT_MULTI = 10000;

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ARTIFACTS_DIR = path.join(HERE, 'artifacts');
const CHECKPOINT_FILE = path.join(ARTIFACTS_DIR, 'neat_checkpoint_inputs_44.json');
const BEST_NETWORK_FILE = path.join(ARTIFACTS_DIR, 'neat_best_inputs_44.json');

interface GenomeWithFitness {
    genome: any;
    fitness: number;
}

function summarizeGenome(genome: any): { hiddenNodes: number; connections: number; avgAbsWeight: number; maxAbsWeight: number } {
    const nodes: any[] = Array.isArray(genome?.nodes) ? genome.nodes : [];
    const conns: any[] = Array.isArray(genome?.connections) ? genome.connections : [];

    const hiddenNodes = nodes.filter(n => n?.type === 'hidden').length;
    const connections = conns.length;

    let sumAbs = 0;
    let maxAbsWeight = 0;
    for (const c of conns) {
        const w = typeof c?.weight === 'number' ? c.weight : 0;
        const a = Math.abs(w);
        sumAbs += a;
        if (a > maxAbsWeight) maxAbsWeight = a;
    }
    const avgAbsWeight = connections > 0 ? sumAbs / connections : 0;

    return { hiddenNodes, connections, avgAbsWeight, maxAbsWeight };
}

function randomChoice<T>(arr: T[]): T | undefined {
    if (!arr || arr.length === 0) return undefined;
    return arr[Math.floor(Math.random() * arr.length)];
}

function maxWinningPlace(playerCount: number): number {
    if (playerCount <= 3) return 1;
    if (playerCount <= 5) return 2;
    if (playerCount <= 7) return 3;
    return 4; // 8 players
}

type SimpleMove = ReturnType<typeof getLegalMoves>[number];

function legalMoveToEngineMove(move: LegalMove): SimpleMove {
    // Some strategies encode "done attacking" as attack+done_attacking_this_round with no cards.
    if (move.type === 'attack' && move.done_attacking_this_round && (!move.cards || move.cards.length === 0)) {
        return { type: 'good' } as any;
    }

    if (move.type === 'attack') {
        const cards = move.cards ?? [];
        if (cards.length > 0) return { type: 'attack', cards } as any;
        return { type: 'good' } as any;
    }

    if (move.type === 'cover') {
        const cards = move.cards ?? [];
        if (cards.length > 0) return { type: 'defend', cards } as any;
        return { type: 'wait' } as any;
    }

    if (move.type === 'pass') {
        const c = move.cards?.[0];
        return c ? ({ type: 'pass', card: c } as any) : ({ type: 'wait' } as any);
    }

    if (move.type === 'pickup') return { type: 'pickup' } as any;
    if (move.type === 'good') return { type: 'good' } as any;
    if (move.type === 'wait') return { type: 'wait' } as any;
    return { type: 'wait' } as any;
}

async function playOneGame(
    strategies: BotStrategy[],
    neatIndex: number,
    seed: string,
    moveLimit: number
): Promise<{ neatWon: boolean; terminal: boolean; finishingOrder: number[] }> {
    const game = initializeGame(strategies.length, seed);
        
        let moveCount = 0;
    while (!isTerminal(game) && moveCount < moveLimit) {
            const currentPlayerIndex = getCurrentPlayer(game);
            if (currentPlayerIndex === -1) break;
            
            const player = game.players[currentPlayerIndex];
            const strategy = strategies[currentPlayerIndex];
            
        // Calculate legal moves (can blow up when hands are large).
        let legalMoves: LegalMove[] = [];
        try {
            legalMoves = calculateLegalMoves(game, player.player_id) as LegalMove[];
        } catch {
            legalMoves = [];
        }

        try {
            if (legalMoves && legalMoves.length > 0) {
                let chosen = legalMoves[0];
                try {
                    chosen = await strategy.chooseMove(game, player.player_id, legalMoves);
                } catch {
                    // If move selection throws (common with huge hands / recursion), just play random.
                    chosen = randomChoice(legalMoves) ?? legalMoves[0];
                }

                // Safety: if the strategy returns something weird, clamp to a legal move.
                if (!legalMoves.includes(chosen)) {
                    chosen = randomChoice(legalMoves) ?? legalMoves[0];
                }

                applyMove(game, currentPlayerIndex, legalMoveToEngineMove(chosen));
            } else {
                // Fallback: keep the game moving with engine-level moves if combinatorics blow up.
                const engineMoves = getLegalMoves(game, currentPlayerIndex);
                const fallback = randomChoice(engineMoves) ?? engineMoves[0];
                if (!fallback) break;
                applyMove(game, currentPlayerIndex, fallback as any);
            }
        } catch {
            // Last-resort: anything in move selection/conversion/applyMove crashed.
            // Choose a random move and continue; these states tend to correlate with low-fitness genomes.
            const engineMoves = getLegalMoves(game, currentPlayerIndex);
            const fallback = randomChoice(engineMoves) ?? engineMoves[0];
            if (!fallback) break;
            try {
                applyMove(game, currentPlayerIndex, fallback as any);
            } catch {
                break;
            }
        }
        moveCount++;
    }

    const terminal = isTerminal(game);
    const finishingOrder = terminal ? getFinishingOrder(game) : [];

    if (!terminal || finishingOrder.length !== strategies.length) {
        // timeout / anomaly -> treat as loss
        return { neatWon: false, terminal, finishingOrder };
    }

    const neatPlace0 = finishingOrder.indexOf(neatIndex); // 0-based
    const maxPlace = maxWinningPlace(strategies.length); // 1-based
    const neatWon = neatPlace0 >= 0 && neatPlace0 < maxPlace;
    return { neatWon, terminal, finishingOrder };
}

async function evaluateGenomeFitness(genome: any, genLabel: string): Promise<{ fitness: number; perSizeWinRate: Record<number, number> }> {
    const neatBot = NEATBotStrategy.fromNetwork(genome, { includePolynomialFeatures: false });
    const handwrittenBot = new HandwrittenBotStrategy();

    const perSizeWinRate: Record<number, number> = {};
    let totalWins = 0;
    let totalGames = 0;

    for (const playerCount of FITNESS_PLAYER_COUNTS) {
        let wins = 0;
        const games = FITNESS_GAMES_PER_SIZE;
        const moveLimit = playerCount <= 2 ? MOVE_LIMIT_2P : MOVE_LIMIT_MULTI;

        for (let g = 0; g < games; g++) {
            const neatIndex = Math.floor(Math.random() * playerCount);
            const strategies: BotStrategy[] = new Array(playerCount).fill(handwrittenBot);
            strategies[neatIndex] = neatBot;

            const seed = `${genLabel}_p${playerCount}_g${g}_seat${neatIndex}`;
            const result = await playOneGame(strategies, neatIndex, seed, moveLimit);
            if (result.neatWon) wins++;
        }

        const winRate = games > 0 ? wins / games : 0;
        perSizeWinRate[playerCount] = winRate;
        totalWins += wins;
        totalGames += games;
    }

    // Fitness granularity becomes 1 / (FITNESS_GAMES_PER_SIZE * number_of_sizes)
    // (e.g. 1/(10*7) when using 10 games per size across 7 sizes).
    const fitness = totalGames > 0 ? totalWins / totalGames : 0;
    return { fitness, perSizeWinRate };
}

function tournamentSelect(population: GenomeWithFitness[], tournamentSize: number): GenomeWithFitness {
    let best = population[Math.floor(Math.random() * population.length)];
    for (let i = 1; i < tournamentSize; i++) {
        const cand = population[Math.floor(Math.random() * population.length)];
        if (cand.fitness > best.fitness) best = cand;
    }
    return best;
}

async function train(): Promise<void> {
    console.log('='.repeat(60));
    console.log('NEAT BOT TRAINING (vs handwritten; fitness=sum winrates 2..8p)');
    console.log('='.repeat(60));
    console.log(`Population: ${POPULATION_SIZE}`);
    console.log(`Generations: ${GENERATIONS}`);
    console.log(`Fitness games per size: ${FITNESS_GAMES_PER_SIZE} (set env FITNESS_GAMES_PER_SIZE)`);
    console.log(`Move limit: 2p=${MOVE_LIMIT_2P}, multi=${MOVE_LIMIT_MULTI}`);
    console.log('='.repeat(60));
    console.log('');
    
    const inputCount = FeatureExtractor.getBaseFeatureCount();
    const outputCount = 1;
    
    let population: GenomeWithFitness[] = [];
    let startGeneration = 0;
    
    let bestEverFitness = -Infinity;
    let bestEverNetwork: any = null;

    if (!existsSync(ARTIFACTS_DIR)) {
        mkdirSync(ARTIFACTS_DIR, { recursive: true });
    }

    if (existsSync(CHECKPOINT_FILE)) {
        console.log('Loading checkpoint...');
        const checkpoint = JSON.parse(readFileSync(CHECKPOINT_FILE, 'utf-8'));
        startGeneration = checkpoint.generation + 1;

        const loadedPopulation = checkpoint.population.map((data: any) => ({
            genome: Network.fromJSON(data.network),
            fitness: 0
        }));

        const sampleNet = loadedPopulation[0]?.genome;
        const inputNodes =
            sampleNet?.nodes?.filter((n: any) => n.type === 'input')?.length ??
            0;
        if (inputNodes !== inputCount) {
            throw new Error(
                `Checkpoint ${CHECKPOINT_FILE} has ${inputNodes} input nodes, but expected ${inputCount} (44-feature run). Delete the file to restart.`
            );
        }

        population = loadedPopulation;
        if (checkpoint.bestEver) {
            bestEverNetwork = Network.fromJSON(checkpoint.bestEver);
        }
        console.log(`Resuming from generation ${startGeneration}`);
    } else {
        for (let i = 0; i < POPULATION_SIZE; i++) {
            const network = new (architect as any).Perceptron(
                inputCount,
                Math.floor(inputCount / 2),
                outputCount
            );
            population.push({ genome: network, fitness: 0 });
        }
    }
    
    // Training loop
    for (let gen = startGeneration; gen < GENERATIONS; gen++) {
        console.log(`\nGeneration ${gen + 1}/${GENERATIONS}`);
        console.log('-'.repeat(60));
        
        // Evaluate each genome vs handwritten across sizes
        for (let i = 0; i < population.length; i++) {
            const { fitness } = await evaluateGenomeFitness(population[i].genome, `gen${gen + 1}_g${i + 1}`);
            population[i].fitness = fitness;

            if (fitness > bestEverFitness) {
                bestEverFitness = fitness;
                bestEverNetwork = population[i].genome;
                console.log(`🎉 New best fitness: ${bestEverFitness.toFixed(3)} (genome ${i + 1}/${population.length})`);
                writeFileSync(BEST_NETWORK_FILE, JSON.stringify(bestEverNetwork.toJSON(), null, 2));
            }

            console.log(`  Genome ${String(i + 1).padStart(3)} fitness: ${fitness.toFixed(3)}`);
        }

        population.sort((a, b) => b.fitness - a.fitness);
        
        const avgFitness = population.reduce((sum, g) => sum + g.fitness, 0) / population.length;
        console.log('');
        console.log(`Generation ${gen + 1} Stats:`);
        console.log(`  Best: ${population[0].fitness.toFixed(3)}`);
        console.log(`  Average: ${avgFitness.toFixed(3)}`);
        console.log(`  Worst: ${population[population.length - 1].fitness.toFixed(3)}`);
        console.log(`  Best Ever: ${bestEverFitness.toFixed(3)}`);

        const bestSummary = summarizeGenome(population[0].genome);
        console.log('  Best Genome Complexity:');
        console.log(`    Hidden Nodes: ${bestSummary.hiddenNodes}`);
        console.log(`    Connections: ${bestSummary.connections}`);
        console.log(`    Avg |Weight|: ${bestSummary.avgAbsWeight.toFixed(3)}`);
        console.log(`    Max |Weight|: ${bestSummary.maxAbsWeight.toFixed(3)}`);
        
        // Save checkpoint
        const checkpoint = {
            generation: gen,
            inputCount,
            population: population.map(g => ({
                network: g.genome.toJSON(),
                fitness: g.fitness
            })),
            bestEver: bestEverNetwork ? bestEverNetwork.toJSON() : null
        };
        writeFileSync(CHECKPOINT_FILE, JSON.stringify(checkpoint, null, 2));

        // Reproduce
        if (gen < GENERATIONS - 1) {
            const nextPopulation: GenomeWithFitness[] = [];
            
            // Elitism: keep top 10%
            const eliteCount = Math.max(1, Math.floor(POPULATION_SIZE * 0.1));
            for (let i = 0; i < eliteCount; i++) {
                nextPopulation.push({ genome: population[i].genome, fitness: 0 });
            }
            
            while (nextPopulation.length < POPULATION_SIZE) {
                const parent1 = tournamentSelect(population, 5);
                const parent2 = tournamentSelect(population, 5);
                
                const child = Network.crossOver(parent1.genome, parent2.genome, true);
                
                const mutationTypes = [
                    methods.mutation.ADD_NODE,
                    methods.mutation.ADD_CONN,
                    methods.mutation.SUB_CONN,
                    methods.mutation.MOD_WEIGHT,
                    methods.mutation.MOD_BIAS,
                    methods.mutation.MOD_ACTIVATION,
                    methods.mutation.ADD_GATE,
                    methods.mutation.ADD_SELF_CONN,
                    methods.mutation.ADD_BACK_CONN
                ];
                
                const numMutations = Math.floor(Math.random() * 8) + 1;
                for (let m = 0; m < numMutations; m++) {
                    const mutationType = mutationTypes[Math.floor(Math.random() * mutationTypes.length)];
                    child.mutate(mutationType);
                }
                
                nextPopulation.push({ genome: child, fitness: 0 });
            }
            
            population = nextPopulation;
        }
    }
    
    console.log('\n' + '='.repeat(60));
    console.log('TRAINING COMPLETE');
    console.log('='.repeat(60));
    console.log(`Best fitness achieved: ${bestEverFitness.toFixed(3)}`);
    console.log(`Best network saved to: ${BEST_NETWORK_FILE}`);
}

train().catch(console.error);


