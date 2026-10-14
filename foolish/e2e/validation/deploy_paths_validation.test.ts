// The Supabase edge functions are deployed with what they load.
//
// The backend moved to c_base on AWS on 2026-10-14, and deploy.yml stopped
// running on push: it is a workflow_dispatch-only lane that keeps Supabase
// deployable for the 30 days old iOS installs still talk to it. The trigger-path
// tests that used to open this file (every file the functions compile had to be
// under deploy.yml's on.push.paths) went with the trigger: a dispatch deploys the
// whole tree. What stays is what a dispatched deploy still depends on: each
// function stages the kernel it loads, and every function resolves its aliases
// from its own identical import map. Delete this file with deploy.yml.
//
// Pure test - no Postgres, no network.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');
// The workflow's globs are written from the repository root, one level above
// the product, so every path compared against them is in THAT coordinate
// system: `foolish/sdk/...`, `shared/c/...`.
const MONO = resolve(REPO, '..');
const SUPA = join(REPO, 'server/impls/supabase');
const FUNCTIONS = join(SUPA, 'functions');

/** The function directories: every one with an index.ts entrypoint. */
const functionNames = readdirSync(FUNCTIONS)
    .filter((name) => existsSync(join(FUNCTIONS, name, 'index.ts')) && statSync(join(FUNCTIONS, name, 'index.ts')).isFile());

/** One function's import map (its deno.json), with each target made absolute. */
function aliasesOf(name: string): Record<string, string> {
    const map = JSON.parse(readFileSync(join(FUNCTIONS, name, 'deno.json'), 'utf8')).imports as Record<string, string>;
    return Object.fromEntries(Object.entries(map).map(([alias, target]) => [alias, resolve(FUNCTIONS, name, target)]));
}

/** `@sdk/` -> `<repo>/sdk/`, exactly as the deployed functions resolve it (the maps are held identical below). */
const aliases: Record<string, string> = functionNames.length > 0 && existsSync(join(FUNCTIONS, functionNames[0], 'deno.json'))
    ? aliasesOf(functionNames[0]) : {};

/** Every quoted specifier in `from '...'`, `import('...')` and `new URL('...')`. */
const SPEC = /(?:from|import|URL)\s*\(?\s*['"]([^'"]+)['"]/g;

function resolveSpec(spec: string, fromFile: string): string | null {
    for (const [alias, target] of Object.entries(aliases)) {
        if (spec.startsWith(alias)) return resolve(target, spec.slice(alias.length));
    }
    if (spec.startsWith('.')) return resolve(dirname(fromFile), spec);
    return null;   // jsr:/https:/npm: - not a file in this repo
}

// ---- the wasm asset each function loads --------------------------------------

/** The files one function's entrypoint reaches, statically or by dynamic import. */
function graphOf(entry: string): Set<string> {
    const seen = new Set<string>();
    const queue = [entry];
    while (queue.length) {
        const f = queue.pop()!;
        if (seen.has(f) || !existsSync(f)) continue;
        seen.add(f);
        for (const m of readFileSync(f, 'utf8').matchAll(SPEC)) {
            const target = resolveSpec(m[1], f);
            if (target && existsSync(target) && !seen.has(target)) queue.push(target);
        }
    }
    return seen;
}

/** config.toml's static_files for one [functions.<name>] section. */
function staticFilesOf(name: string): string[] {
    const toml = readFileSync(join(SUPA, 'config.toml'), 'utf8').split('\n');
    const start = toml.findIndex((l) => l.trim() === `[functions.${name}]`);
    if (start < 0) return [];
    const out: string[] = [];
    for (const line of toml.slice(start + 1)) {
        const t = line.trim();
        if (t.startsWith('[')) break;
        if (!t.startsWith('static_files')) continue;
        for (const m of t.matchAll(/"([^"]+)"/g)) out.push(relative(MONO, resolve(SUPA, m[1])));
    }
    return out;
}

test('the edge asks for the module config.toml stages', async () => {
    // The kernel has two links now (c/Makefile, WASM_WEB_NAMES): bots.wasm for a
    // runtime with a filesystem, web.wasm for the browser, which fetches. The
    // choice is made in code - kernelModule() - while the file that gets
    // UPLOADED is named in config.toml, and nothing connects the two but this.
    //
    // Getting it wrong is invisible until production: a `web` answer here would
    // make every edge function ask for a file the deploy never staged, and the
    // first request after the deploy would be the first anyone heard of it.
    const { kernelModule } = await import('../../sdk/ts/wasm/wasm_asset.ts');
    assert.equal(kernelModule(), 'bots', 'Node has a filesystem, so it must read the staged module');

    // And the runtime that actually runs there. `Deno` is absent under Node, so
    // the branch is otherwise never exercised by any test in this repo.
    const g = globalThis as Record<string, unknown>;
    const had = 'Deno' in g, prev = g.Deno;
    g.Deno = { readFileSync: () => new Uint8Array() };
    try {
        assert.equal(kernelModule(), 'bots',
            'the Deno edge would load a module config.toml does not stage - every function would 500');
    } finally {
        if (had) g.Deno = prev; else delete g.Deno;
    }

    for (const name of readdirSync(FUNCTIONS)) {
        for (const f of staticFilesOf(name)) {
            assert.ok(!f.endsWith('/web.wasm.gz'),
                `${name} stages the browser's link of the kernel; the edge reads ${kernelModule()}.wasm`);
        }
    }
});

test('every function that loads bots.wasm bundles it (config.toml static_files)', () => {
    // The C Table runs in every function that touches a game (Phase 4b of
    // docs/C_GAME_SHAPE_MIGRATION.md): create and delete-account load the module
    // too now, and a deploy without the asset fails on the first request.
    const loader = join(REPO, 'sdk/ts/wasm/wasm_asset.ts');
    const missing: string[] = [];
    for (const name of readdirSync(FUNCTIONS)) {
        const entry = join(FUNCTIONS, name, 'index.ts');
        if (!existsSync(entry) || !graphOf(entry).has(loader)) continue;
        if (!staticFilesOf(name).includes('foolish/sdk/ts/wasm/bots.wasm.gz')) missing.push(name);
    }
    assert.deepEqual(missing, [], `these functions load bots.wasm without bundling it: ${missing.join(', ')}`);
});

// ---- the import map each function is served and deployed with -----------------

test('every function has its own deno.json import map, config.toml names it, and all resolve alike', () => {
    // `supabase functions serve` resolves the aliases only from the function's own
    // deno.json: with one shared import_map.json every local worker failed to boot.
    // config.toml's import_map points `deploy` at the same file, so one map serves
    // both, and the maps must not drift apart function by function.
    const toml = readFileSync(join(SUPA, 'config.toml'), 'utf8').split('\n');
    const problems: string[] = [];
    for (const name of functionNames) {
        if (!existsSync(join(FUNCTIONS, name, 'deno.json'))) { problems.push(`${name}: no deno.json`); continue; }
        const start = toml.findIndex((l) => l.trim() === `[functions.${name}]`);
        const rest = start < 0 ? [] : toml.slice(start + 1);
        const end = rest.findIndex((l) => l.trim().startsWith('['));
        const section = end < 0 ? rest : rest.slice(0, end);
        const named = section.map((l) => l.trim().match(/^import_map\s*=\s*"([^"]+)"/)?.[1]).find(Boolean);
        if (!named || resolve(SUPA, named) !== join(FUNCTIONS, name, 'deno.json')) problems.push(`${name}: config.toml import_map is ${named ?? 'missing'}`);
        const mine = aliasesOf(name);
        if (JSON.stringify(mine) !== JSON.stringify(aliases)) problems.push(`${name}: its aliases differ: ${JSON.stringify(mine)}`);
        for (const [alias, target] of Object.entries(mine)) {
            if (!existsSync(target)) problems.push(`${name}: ${alias} resolves to a missing ${relative(MONO, target)}`);
        }
    }
    assert.ok(functionNames.length > 0 && Object.keys(aliases).length > 0, 'no functions or no aliases found');
    assert.deepEqual(problems, []);
});
