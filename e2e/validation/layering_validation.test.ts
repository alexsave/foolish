// A10 — the module layering is a machine-checked invariant, not a hope.
//
// _shared/ is four tiers with a one-way dependency DAG:
//
//     core   (0) — domain vocabulary, zero logic, zero host deps
//     sdk    (1) — the ONLY code that knows the C kernel's ABI (wasm/wire)
//     common (2) — host-neutral game logic
//     adapter(3) — the Supabase host: DB, realtime, lease, auth, waitUntil
//
// A file may import its own tier or any tier BENEATH it, never above. And
// core/sdk/common must carry NO vendor/host coupling — that is what makes the
// adapter swappable "with no diff in the common part" (the A10 acceptance
// test). This test fails the build the moment either rule is broken.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';

const SHARED = resolve(import.meta.dirname, '../../supabase/functions/_shared');
const RANK: Record<string, number> = { core: 0, sdk: 1, common: 2, adapter: 3 };

function layerOf(absPath: string): string | null {
    const rel = relative(SHARED, absPath);
    if (rel.startsWith('..')) return null;          // outside _shared
    const top = rel.split('/')[0];
    return top in RANK ? top : null;
}

function walk(dir: string): string[] {
    const out: string[] = [];
    for (const name of readdirSync(dir)) {
        const p = join(dir, name);
        if (statSync(p).isDirectory()) out.push(...walk(p));
        else if (/\.(ts|tsx|mts)$/.test(name)) out.push(p);
    }
    return out;
}

// every quoted specifier in from '...' / import('...') / new URL('...')
const SPEC = /(?:from|import|URL)\s*\(?\s*['"]([^'"]+)['"]/g;

const files = walk(SHARED);

test('the four tiers form a one-way DAG (no file imports a higher tier)', () => {
    const violations: string[] = [];
    for (const f of files) {
        const fromLayer = layerOf(f);
        if (!fromLayer) continue;
        const src = readFileSync(f, 'utf8');
        for (const m of src.matchAll(SPEC)) {
            const spec = m[1];
            if (!spec.startsWith('.')) continue;    // external / npm / jsr — not a local tier edge
            const target = resolve(dirname(f), spec);
            const toLayer = layerOf(target);
            if (!toLayer) continue;                 // resolves outside _shared
            if (RANK[toLayer] > RANK[fromLayer]) {
                violations.push(
                    `${relative(SHARED, f)} (${fromLayer}) imports ${spec} (${toLayer}) — upward edge`);
            }
        }
    }
    assert.deepEqual(violations, [], `\n${violations.join('\n')}\n`);
});

test('core / sdk / common carry no vendor or host coupling', () => {
    // The tokens that mean "this code is bound to a specific backend/runtime".
    // Deno.readFileSync is deliberately NOT here: the SDK probes globalThis.Deno
    // to load its wasm asset across runtimes — a capability probe, not coupling.
    const FORBIDDEN = [/jsr:@supabase/, /\bEdgeRuntime\b/, /\bDeno\.env\b/, /\bDeno\.serve\b/, /\bcreateClient\b/];
    const violations: string[] = [];
    for (const f of files) {
        const layer = layerOf(f);
        if (!layer || layer === 'adapter') continue;   // the adapter is ALLOWED to couple
        const src = readFileSync(f, 'utf8');
        for (const pat of FORBIDDEN) {
            if (pat.test(src)) {
                violations.push(`${relative(SHARED, f)} (${layer}) contains ${pat} — host coupling above the adapter`);
            }
        }
    }
    assert.deepEqual(violations, [], `\n${violations.join('\n')}\n`);
});

test('every tier is non-empty (the split actually happened)', () => {
    const counts: Record<string, number> = { core: 0, sdk: 0, common: 0, adapter: 0 };
    for (const f of files) {
        const l = layerOf(f);
        if (l) counts[l]++;
    }
    for (const tier of Object.keys(RANK)) {
        assert.ok(counts[tier] > 0, `tier '${tier}' has no files — did the layout move?`);
    }
});
