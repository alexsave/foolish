// Hill-climb trainer for nitro's parametric weights.
//
// Goal: maximize wins over a contiguous seed range (default 1..1000) vs
// espresso. Each iteration perturbs one weight, runs the full sweep, and
// keeps the perturbation iff total wins do not decrease (strict improvements
// are saved to disk; ties accepted with small probability for drift).
//
// Usage:
//   tsx offlinefun/localtest/nitro_train.ts \
//     --from=1 --to=1000 --iters=2000 \
//     --out=supabase/functions/_shared/strategies/nitro_weights.json
//
// Resume by passing --in=<same path> (defaults to --out).

import { registerBotStrategy } from '../../supabase/functions/_shared/bot_strategy.ts';
import { createGame, normSeed, runBotsToCompletion } from './harness.ts';
import { game_done } from '../../supabase/functions/_shared/common_utils.ts';
import { start_game } from '../../supabase/functions/_shared/game_lifecycle.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, StrategyKey } from '../../supabase/functions/_shared/types.ts';
import { EspressoStrategy } from './frozen/espresso_strategy.ts';
import {
    NitroStrategy,
    NitroWeights,
    setNitroWeights,
    makeDefaultWeights,
    DECK_BUCKETS,
    NUM_VALUES,
} from '../../supabase/functions/_shared/strategies/nitro_strategy.ts';
import { setRandomSeed } from './frozen/random_strategy.ts';
import * as fs from 'node:fs';

const NITRO = 'nitro' as StrategyKey;
const ESPRESSO = 'espresso' as StrategyKey;
registerBotStrategy(NITRO, new NitroStrategy());
registerBotStrategy(ESPRESSO, new EspressoStrategy());

let _seed = 1;
const seededRandom = () => {
    _seed = (_seed * 1664525 + 1013904223) % 4294967296;
    return _seed / 4294967296;
};
Math.random = seededRandom;

const noop = () => { };
console.log = noop; console.warn = noop; console.error = noop; console.info = noop;
const print = (...args: any[]) => fs.writeSync(1, args.map(a => typeof a === 'string' ? a : JSON.stringify(a)).join(' ') + '\n');

async function nitroWinsSeed(seed: number, capIters = 2000): Promise<boolean> {
    _seed = normSeed(seed);
    setRandomSeed(normSeed(seed));
    const game = createGame([NITRO, ESPRESSO]);
    start_game(game);
    await runBotsToCompletion(game, capIters);
    const loser = game_done(game);
    return loser !== null && loser !== 'bot_0_nitro';
}

// --- weight (de)serialization -----------------------------------------------

function cloneWeights(w: NitroWeights): NitroWeights {
    return {
        cardWeights: w.cardWeights.map(arr2 => arr2.map(arr1 => [...arr1])),
        pickupPerCard: [...w.pickupPerCard],
        coverGapPenalty: [...w.coverGapPenalty],
    };
}

function loadWeightsFile(path: string): NitroWeights | null {
    try {
        const raw = fs.readFileSync(path, 'utf8');
        const j = JSON.parse(raw);
        if (
            !Array.isArray(j.cardWeights)
            || !Array.isArray(j.pickupPerCard)
            || !Array.isArray(j.coverGapPenalty)
        ) {
            return null;
        }
        return j as NitroWeights;
    } catch {
        return null;
    }
}

function saveWeightsFile(path: string, w: NitroWeights): void {
    fs.writeFileSync(path, JSON.stringify(w, null, 2));
}

// --- evaluation -------------------------------------------------------------

async function evaluate(seeds: number[]): Promise<{ wins: number; lossSeeds: number[] }> {
    let wins = 0;
    const lossSeeds: number[] = [];
    for (const s of seeds) {
        const win = await nitroWinsSeed(s);
        if (win) wins++;
        else lossSeeds.push(s);
    }
    return { wins, lossSeeds };
}

// --- perturbation -----------------------------------------------------------

type Perturb = {
    kind: 'card' | 'pickup' | 'gap';
    deck: number;
    isTrump?: number;
    valueIdx?: number;
    delta: number;
};

function describePerturb(p: Perturb): string {
    if (p.kind === 'card') {
        return `card[deck=${p.deck}][${p.isTrump === 1 ? 'T' : 'N'}][v=${(p.valueIdx ?? 0) + 5}] += ${p.delta.toFixed(2)}`;
    }
    if (p.kind === 'pickup') return `pickup[deck=${p.deck}] += ${p.delta.toFixed(2)}`;
    return `gap[deck=${p.deck}] += ${p.delta.toFixed(2)}`;
}

function applyPerturb(w: NitroWeights, p: Perturb): NitroWeights {
    const c = cloneWeights(w);
    if (p.kind === 'card') {
        c.cardWeights[p.deck][p.isTrump!][p.valueIdx!] += p.delta;
    } else if (p.kind === 'pickup') {
        c.pickupPerCard[p.deck] = Math.max(1, c.pickupPerCard[p.deck] + p.delta);
    } else {
        c.coverGapPenalty[p.deck] = Math.max(0, c.coverGapPenalty[p.deck] + p.delta);
    }
    return c;
}

function randomPerturb(rng: () => number, scale: number): Perturb {
    const deck = Math.floor(rng() * DECK_BUCKETS);
    const r = rng();
    const delta = (rng() * 2 - 1) * scale;
    if (r < 0.7) {
        // Most perturbations to per-card weights.
        return {
            kind: 'card',
            deck,
            isTrump: rng() < 0.5 ? 0 : 1,
            valueIdx: Math.floor(rng() * NUM_VALUES),
            delta,
        };
    }
    if (r < 0.9) {
        return { kind: 'pickup', deck, delta: delta * 5 };
    }
    return { kind: 'gap', deck, delta: delta * 0.5 };
}

// --- argument parsing -------------------------------------------------------

function parseList(s: string): number[] {
    return s.split(',').map(x => parseInt(x.trim(), 10)).filter(x => !isNaN(x));
}

const args = process.argv.slice(2);
const argMap = new Map<string, string>();
for (const a of args) {
    if (a.startsWith('--')) {
        const eq = a.indexOf('=');
        if (eq > 0) argMap.set(a.slice(2, eq), a.slice(eq + 1));
        else argMap.set(a.slice(2), 'true');
    }
}

const trainFrom = parseInt(argMap.get('from') ?? '1', 10);
const trainTo = parseInt(argMap.get('to') ?? '1000', 10);
const iters = parseInt(argMap.get('iters') ?? '2000', 10);
const outPath = argMap.get('out') ?? 'supabase/functions/_shared/strategies/nitro_weights.json';
const inPath = argMap.get('in') ?? outPath;
const initialScale = parseFloat(argMap.get('scale') ?? '15');
const trainSeeds: number[] = [];
for (let s = trainFrom; s <= trainTo; s++) trainSeeds.push(s);

// --- training loop ----------------------------------------------------------

(async () => {
    const initial = loadWeightsFile(inPath) ?? makeDefaultWeights();
    let best = cloneWeights(initial);
    setNitroWeights(best);

    const initialEval = await evaluate(trainSeeds);
    let bestWins = initialEval.wins;
    saveWeightsFile(outPath, best);

    print(`# nitro_train start: seeds=${trainFrom}..${trainTo} (${trainSeeds.length} seeds), iters=${iters}`);
    print(`# initial wins=${bestWins}/${trainSeeds.length}`);

    let scale = initialScale;
    const start = Date.now();
    let lastSaved = bestWins;

    for (let iter = 1; iter <= iters; iter++) {
        const p = randomPerturb(seededRandom, scale);
        const candidate = applyPerturb(best, p);
        setNitroWeights(candidate);

        const r = await evaluate(trainSeeds);
        // Strict gain → accept and save. Tie → small chance of drift.
        const accept = r.wins > bestWins
            || (r.wins === bestWins && Math.random() < 0.05);

        if (accept) {
            best = candidate;
            if (r.wins > bestWins) {
                bestWins = r.wins;
                saveWeightsFile(outPath, best);
                lastSaved = bestWins;
                const dt = ((Date.now() - start) / 1000).toFixed(1);
                print(`[${iter}] +UP wins=${bestWins}/${trainSeeds.length} ${describePerturb(p)} dt=${dt}s`);
            }
        } else {
            setNitroWeights(best);
        }

        if (iter % 200 === 0) {
            const dt = ((Date.now() - start) / 1000).toFixed(1);
            print(`[${iter}] wins=${bestWins}/${trainSeeds.length} scale=${scale.toFixed(1)} dt=${dt}s`);
            // Defensive save on the round number even if no strict improvement.
            if (bestWins > lastSaved) {
                saveWeightsFile(outPath, best);
                lastSaved = bestWins;
            }
        }

        scale = Math.max(1, scale * 0.9995);
    }

    saveWeightsFile(outPath, best);
    const dt = ((Date.now() - start) / 1000).toFixed(1);
    print(`# end: wins=${bestWins}/${trainSeeds.length} dt=${dt}s`);
})();
