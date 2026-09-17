// fuzz_moves.ts - the seeded RNG the fuzz suites draw their hostile inputs from.
//
// A test file cannot be imported for its helpers (importing it registers its
// tests), so the stream e2e/table_parity.test.ts draws its sweeps from lives here.
// It draws from an explicit seed, so a found exploit reproduces exactly.
//
// The adversarial request generators that drew from a TypeScript Game are gone
// with the TS game shape (docs/C_GAME_SHAPE_MIGRATION.md Phase 8); nothing called
// them since e2e/fuzz.test.ts moved to its own table-shaped generators and
// table_parity to recorded requests (Phase 4b).

export interface FuzzRng {
    rnd(): number;
    ri(n: number): number;
    pick<T>(a: T[]): T;
}

// Deterministic LCG, so a found exploit reproduces from the printed seed.
export function fuzzRng(seed: number): FuzzRng {
    let s = seed >>> 0;
    const rnd = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 0x100000000; };
    const ri = (n: number) => Math.floor(rnd() * n);
    return { rnd, ri, pick: <T>(a: T[]): T => a[ri(a.length)] };
}
