// Train the nitro transformer policy on a corpus of (tokens, target, legal)
// triples emitted by nitro_collect.
//
// Usage:
//   tsx offlinefun/localtest/nitro_train_nn.ts \
//     --corpus=/tmp/nitro_corpus.jsonl \
//     --out=supabase/functions/_shared/strategies/nitro_nn_weights.json \
//     --epochs=5 --batch=32 --lr=0.01

import * as fs from 'node:fs';
import {
    makeRandomParams,
    forward,
    accumulateGrads,
    applyGrads,
    makeZeroGrads,
    softmaxMasked,
    serializeParams,
    deserializeParams,
    NUM_ACTIONS,
} from '../../supabase/functions/_shared/strategies/nitro_nn.ts';

interface Sample {
    tokens: number[];
    target: number;
    legal: number[]; // list of legal action ids
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
const corpusPath = argMap.get('corpus') ?? '/tmp/nitro_corpus.jsonl';
const outPath = argMap.get('out') ?? 'supabase/functions/_shared/strategies/nitro_nn_weights.json';
const inPath = argMap.get('in');
const epochs = parseInt(argMap.get('epochs') ?? '5', 10);
const batch = parseInt(argMap.get('batch') ?? '32', 10);
const lr = parseFloat(argMap.get('lr') ?? '0.01');
const seed = parseInt(argMap.get('seed') ?? '42', 10);

const print = (...args: any[]) => fs.writeSync(1, args.map(a => typeof a === 'string' ? a : JSON.stringify(a)).join(' ') + '\n');

print(`# loading corpus from ${corpusPath}`);
const raw = fs.readFileSync(corpusPath, 'utf8');
const lines = raw.split('\n').filter(l => l.length > 0);
const samples: Sample[] = lines.map(l => JSON.parse(l));
print(`# loaded ${samples.length} samples`);

// Action distribution sanity-check.
const actionCounts = new Array(NUM_ACTIONS).fill(0);
for (const s of samples) actionCounts[s.target]++;
print(`# action histogram: pickup=${actionCounts[40]} stop=${actionCounts[41]} cards=${actionCounts.slice(0, 40).reduce((a, b) => a + b, 0)}`);

let params = inPath
    ? (deserializeParams(fs.readFileSync(inPath, 'utf8')) ?? makeRandomParams(seed))
    : makeRandomParams(seed);
print(`# init: ${inPath ? `from ${inPath}` : 'fresh'} seed=${seed}`);

const grads = makeZeroGrads();

// Simple Fisher-Yates shuffle with seeded RNG.
let _seed = seed;
const rng = () => {
    _seed = (_seed * 1664525 + 1013904223) >>> 0;
    return _seed / 4294967296;
};
const shuffleInPlace = <T>(arr: T[]) => {
    for (let i = arr.length - 1; i > 0; i--) {
        const j = Math.floor(rng() * (i + 1));
        [arr[i], arr[j]] = [arr[j], arr[i]];
    }
};

const start = Date.now();
let bestEpochLoss = Infinity;

for (let epoch = 1; epoch <= epochs; epoch++) {
    shuffleInPlace(samples);
    let totalLoss = 0;
    let correct = 0;
    let processed = 0;

    for (let bStart = 0; bStart < samples.length; bStart += batch) {
        const bEnd = Math.min(samples.length, bStart + batch);
        let batchLoss = 0;
        for (let i = bStart; i < bEnd; i++) {
            const s = samples[i];
            const legalMask = new Array(NUM_ACTIONS).fill(false);
            for (const a of s.legal) legalMask[a] = true;
            const cache = forward(params, s.tokens);
            // Track top-1 accuracy.
            const probs = softmaxMasked(cache.logits, legalMask);
            let bestA = 0; let bestP = -Infinity;
            for (let a = 0; a < NUM_ACTIONS; a++) {
                if (probs[a] > bestP) { bestP = probs[a]; bestA = a; }
            }
            if (bestA === s.target) correct++;
            const loss = accumulateGrads(params, cache, legalMask, s.target, grads);
            batchLoss += loss;
            processed++;
        }
        applyGrads(params, grads, lr, bEnd - bStart);
        totalLoss += batchLoss;

        if (bStart % (batch * 50) === 0) {
            const dt = ((Date.now() - start) / 1000).toFixed(1);
            print(`  epoch ${epoch} step ${bStart}/${samples.length} avgLoss=${(batchLoss / (bEnd - bStart)).toFixed(4)} dt=${dt}s`);
        }
    }
    const avgLoss = totalLoss / Math.max(1, processed);
    const acc = correct / Math.max(1, processed);
    const dt = ((Date.now() - start) / 1000).toFixed(1);
    print(`# epoch ${epoch}/${epochs}: avgLoss=${avgLoss.toFixed(4)} top1=${(acc * 100).toFixed(1)}% dt=${dt}s`);

    fs.writeFileSync(outPath, serializeParams(params));
    if (avgLoss < bestEpochLoss) bestEpochLoss = avgLoss;
}

print(`# done. best avgLoss=${bestEpochLoss.toFixed(4)}, weights saved to ${outPath}`);
