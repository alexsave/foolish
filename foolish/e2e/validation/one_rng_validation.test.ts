// Every generator in the repo's C is shared/c/rng.h's.
//
// The repo had grown more than sixty hand-rolled generators: the same xorshift
// written out in five bots, the same splitmix64 in four products, three copies
// of one seed derivation in three tools, a test's LCG pasted into the next
// test. Some of those sequences are re-derived from data already in people's
// hands (the legacy deal and the strategy stream from a stored seed, a die's
// throw from a game's seed), so a copy that drifts is not a style problem: it
// is a deal or a die that one build computes differently from another.
//
// rng.h holds each algorithm once and rng_test.c pins it by its published
// vectors. This gate keeps it that way: it refuses, in any C file outside
// those two, the step of a generator rng.h already has - the xorshift shift
// triples, splitmix64's finaliser constant, the LCG multipliers, and the
// xorshift64* output multiplier used as a multiplier. A seed CONSTANT is fine
// (0x2545f4914f6cdd1d as a starting state is just a number); a hash of a key
// is fine (a fingerprint, a book's bucket, a texture's noise): it is a step
// that is refused, wherever it is spelled.
//
// Comments are stripped first, so a comment may name the recurrence it uses.
//
// Pure test - no Postgres, no network, no build.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, resolve, relative } from 'node:path';

const REPO = resolve(import.meta.dirname, '../../..');
const SKIP_DIR = new Set(['build', 'gen', 'node_modules', '__pycache__', '.git', 'DerivedData', '.claude']);

/** The two files that are allowed to spell a generator: its home and its vectors. */
const HOME = new Set(['shared/c/rng.h', 'shared/c/rng_test.c']);

function cFiles(dir: string): string[] {
    const out: string[] = [];
    for (const entry of readdirSync(dir)) {
        const p = join(dir, entry);
        const st = statSync(p);
        if (st.isDirectory()) {
            if (!SKIP_DIR.has(entry)) out.push(...cFiles(p));
        } else if (/\.(c|h|m)$/.test(entry)) {
            out.push(p);
        }
    }
    return out;
}

/** The source with its comments blanked and its whitespace folded to one space. */
function code(src: string): string {
    return src
        .replace(/\/\*[\s\S]*?\*\//g, ' ')
        .replace(/\/\/[^\n]*/g, ' ')
        .replace(/\s+/g, ' ');
}

/** A generator's step, as it is written when someone writes it out by hand. */
const STEP: [string, RegExp][] = [
    ['xorshift64 (13,7,17)', /<< ?13 ?;[^;]*>> ?7 ?;[^;]*<< ?17\b/],
    ['xorshift32 (13,17,5)', /<< ?13 ?;[^;]*>> ?17 ?;[^;]*<< ?5\b/],
    ['xorshift64* (12,25,27)', />> ?12 ?;[^;]*<< ?25 ?;[^;]*>> ?27\b/],
    ["splitmix64's finaliser", /0xbf58476d1ce4e5b9/i],
    ['the 32-bit LCG (Numerical Recipes)', /\* ?1664525u?\b/],
    ['the ANSI C LCG', /\* ?1103515245u?\b/],
    ['the 64-bit LCG (Knuth MMIX / PCG)', /6364136223846793005|0x5851f42d4c957f2d/i],
    ["xorshift64*'s multiplier", /\* ?(2685821657736338717|0x2545f4914f6cdd1d)/i],
];

test('no C file outside shared/c/rng.h writes out a generator', () => {
    const files = ['foolish', 'uttt', 'werewolf', 'shared', 'c_base', 'pickemup', 'chuiniu', 'tallybones']
        .flatMap((d) => cFiles(join(REPO, d)));
    assert.ok(files.length > 500, `found only ${files.length} C files - did the tree move?`);
    const bad: string[] = [];
    for (const f of files) {
        const rel = relative(REPO, f);
        if (HOME.has(rel)) continue;
        const text = code(readFileSync(f, 'utf8'));
        for (const [name, re] of STEP) if (re.test(text)) bad.push(`${rel}: ${name}`);
    }
    assert.deepEqual(bad, [], 'call rng.h instead (#include the shared header by its relative path)');
});

test('the gate sees each step it refuses', () => {
    // Each pattern against the way it was actually written before rng.h, so a
    // pattern that stops matching anything cannot pass silently.
    const seen: Record<string, string> = {
        'xorshift64 (13,7,17)': 'x ^= x << 13;\n    x ^= x >> 7;\n    x ^= x << 17;',
        'xorshift32 (13,17,5)': 's ^= s << 13; s ^= s >> 17; s ^= s << 5;',
        'xorshift64* (12,25,27)': 'RS ^= RS >> 12; RS ^= RS << 25; RS ^= RS >> 27;',
        "splitmix64's finaliser": 'z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;',
        'the 32-bit LCG (Numerical Recipes)': 'g_seed = g_seed * 1664525u + 1013904223u;',
        'the ANSI C LCG': '*rng = *rng * 1103515245u + 12345u;',
        'the 64-bit LCG (Knuth MMIX / PCG)': 'rs = rs * 6364136223846793005ull + 1442695040888963407ull;',
        "xorshift64*'s multiplier": 'return x * 2685821657736338717ull;',
    };
    for (const [name, re] of STEP) {
        assert.ok(seen[name], `no example for ${name}`);
        assert.ok(re.test(code(seen[name])), `${name} does not match how it is written`);
    }
    // ...and a comment naming a recurrence, or a seed constant, is not a step.
    assert.ok(!STEP.some(([, re]) => re.test(code('// state = state * 1664525 + 1013904223\nuint64_t rs = 0x2545f4914f6cdd1dull;'))));
});
