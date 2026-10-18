// Every generator in the repo's C is shared/c/rng.h's, and rng.h has one.
//
// The repo had grown more than sixty hand-rolled generators: the same xorshift
// written out in five bots, the same splitmix64 in four products, three copies
// of one seed derivation in three tools, a test's LCG pasted into the next
// test. PR #321 moved each algorithm into rng.h; the pass after it reduced the
// five algorithms to one, splitmix64, because none of the other four bought
// anything where security does not matter.
//
// The ChaCha20 deal stream (shared/c/deal_rng.h) is the one other generator,
// and it is not here for compatibility: a player sees some of its outputs, and
// splitmix64 can be run backwards from one. No generator is kept for an old
// format's sake; the owner accepted that old uttt bubbles and links redraw
// their pen strokes from the new dice.
//
// The gates:
// - no C file outside rng.h and rng_test.c writes out a generator's step (the
//   xorshift shift triples, splitmix64's finaliser constant, the LCG and
//   Lehmer multipliers, xorshift64*'s output multiplier used as one). A seed
//   CONSTANT is fine (0x2545f4914f6cdd1d as a starting state is just a
//   number); a hash of a key is fine (a fingerprint, a book's bucket, a
//   texture's noise): it is a step that is refused, wherever it is spelled.
// - rng.h defines splitmix64 and its helpers and nothing else: a second
//   algorithm fails here first.
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

/** The one algorithm and the helpers over it: everything rng.h may define. */
const ONE = ['rng_splitmix64_at', 'rng_splitmix64', 'rng_u32', 'rng_seed64', 'rng_unit'];

const PRODUCTS = ['foolish', 'uttt', 'werewolf', 'shared', 'c_base', 'pickemup', 'chuiniu', 'tallybones'];

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
    ["the Lehmer step (MINSTD's multiplier, rough.js)", /\* ?48271u?\b|48271u? ?\*/],
    ["xorshift64*'s multiplier", /\* ?(2685821657736338717|0x2545f4914f6cdd1d)/i],
];

test('no C file outside shared/c/rng.h writes out a generator', () => {
    const files = PRODUCTS.flatMap((d) => cFiles(join(REPO, d)));
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
        "the Lehmer step (MINSTD's multiplier, rough.js)": 'o->seed = (int32_t)((uint32_t)o->seed * 48271u);',
        "xorshift64*'s multiplier": 'return x * 2685821657736338717ull;',
    };
    for (const [name, re] of STEP) {
        assert.ok(seen[name], `no example for ${name}`);
        assert.ok(re.test(code(seen[name])), `${name} does not match how it is written`);
    }
    // ...and a comment naming a recurrence, or a seed constant, is not a step.
    assert.ok(!STEP.some(([, re]) => re.test(code('// state = state * 1664525 + 1013904223\nuint64_t rs = 0x2545f4914f6cdd1dull;'))));
});

/** Every rng_* function rng.h defines. */
function defined(): string[] {
    const src = code(readFileSync(join(REPO, 'shared/c/rng.h'), 'utf8'));
    return [...src.matchAll(/static inline [a-z0-9_]+ (rng_[a-z0-9_]+) ?\(/g)].map((m) => m[1]);
}

test('rng.h defines one algorithm and its helpers, and nothing else', () => {
    assert.deepEqual(defined().sort(), [...ONE].sort(),
        'a second algorithm in rng.h: draw from splitmix64 instead (a sub-stream through rng_seed64)');
});
