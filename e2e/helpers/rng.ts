/* =============================================================================
 * Seeded randomness for e2e suites
 * =============================================================================
 * The invariant the product runs on: the ONLY true nondeterministic draw is the
 * one crypto draw per game, at the deal (injectDealSeed, sdk/ts/wasm/engine.ts).
 * Mid-game engine randomness and bot decisions are both reseeded from it, so a
 * whole game replays from the deal seed.
 *
 * A suite that shuffles with Math.random breaks the same rule from the outside:
 * every run is a different experiment, and a red one hands the reader no repro.
 * So suites draw from the LCG here instead, seeded from the environment:
 *
 *   E2E_SEED_<SUITE>=<n>   this suite only
 *   E2E_SEED=<n>           every suite
 *   neither                DEFAULT_SEED
 *
 * suiteRng() prints the seed it resolved, so a red CI log carries its own repro
 * line. Name the seed in failure messages too: the printed line scrolls away,
 * the assertion text is what gets pasted.
 *
 * Seeding must not shrink what a suite explores. Same trial count, same shapes,
 * just reproducible. To widen the search deliberately, run a range of seeds.
 *
 * The stream is the kernel's own strategy LCG (c/src/game.c
 * random_strategy_random), so a seed pinned here and a seed handed to the
 * kernel walk the same numbers.
 * ========================================================================== */

/* Picked as the date this pass landed, before any suite had been run under it.
 * Not chosen because it is green. */
export const DEFAULT_SEED = 20260905;

/** The kernel's strategy LCG as a [0,1) stream. */
export const mkLcg = (seed: number): (() => number) => {
    let s = (seed >>> 0) || 1;
    return () => {
        s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
        return s / 4294967296;
    };
};

/** The same LCG as a raw u32 stream (seed material for other streams). */
export const mkLcgU32 = (seed: number): (() => number) => {
    let s = (seed >>> 0) || 1;
    return () => {
        s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
        return s;
    };
};

export interface SeededRng {
    /** The suite seed, on a fork too. This is the number to put in a message. */
    readonly seed: number;
    /** The env var that overrides it, e.g. E2E_SEED_RECONCILE. */
    readonly env: string;
    /** [0,1), the raw stream. */
    next(): number;
    /** 0..n-1. */
    int(n: number): number;
    pick<T>(a: T[]): T;
    /** Fisher-Yates, in place, returns the same array. */
    shuffle<T>(a: T[]): T[];
    chance(p: number): boolean;
    /**
     * An independent stream derived from the suite seed and `label`. Needed
     * wherever draws interleave nondeterministically - concurrent games racing
     * on one Postgres share a clock, not a stream, so one stream between them
     * would hand each run a different assignment from the same seed.
     */
    fork(label: string | number): SeededRng;
}

const announced = new Set<string>();

function resolveSeed(env: string, fallback: number): number {
    const raw = process.env[env] ?? process.env.E2E_SEED;
    if (raw === undefined || raw === '') return fallback;
    const n = Number(raw);
    if (!Number.isFinite(n)) throw new Error(`${env}: not a number: ${JSON.stringify(raw)}`);
    return n >>> 0;
}

// FNV-1a, so a fork label mixes into the stream without colliding on order.
const mix = (seed: number, label: string): number => {
    let h = (2166136261 ^ seed) >>> 0;
    for (let i = 0; i < label.length; i++) h = Math.imul(h ^ label.charCodeAt(i), 16777619);
    return h >>> 0;
};

function makeRng(seed: number, env: string, stream: number): SeededRng {
    const next = mkLcg(stream);
    const int = (n: number) => Math.floor(next() * n);
    return {
        seed,
        env,
        next,
        int,
        pick: <T>(a: T[]): T => a[int(a.length)],
        shuffle: <T>(a: T[]): T[] => {
            for (let i = a.length - 1; i > 0; i--) {
                const j = int(i + 1);
                [a[i], a[j]] = [a[j], a[i]];
            }
            return a;
        },
        chance: (p: number) => next() < p,
        fork: (label: string | number) => makeRng(seed, env, mix(stream, String(label))),
    };
}

/**
 * The seeded stream for one suite. `suite` names it in the env var and in the
 * line printed at import: `[seed] reconcile = 20260905 (E2E_SEED_RECONCILE)`.
 */
export function suiteRng(suite: string, fallback: number = DEFAULT_SEED): SeededRng {
    const env = `E2E_SEED_${suite.toUpperCase().replace(/[^A-Z0-9]+/g, '_')}`;
    const seed = resolveSeed(env, fallback);
    if (!announced.has(env)) {
        announced.add(env);
        // Not console.log: several suites silence that unless E2E_VERBOSE.
        process.stdout.write(`[seed] ${suite} = ${seed} (${env})\n`);
    }
    return makeRng(seed, env, seed);
}
