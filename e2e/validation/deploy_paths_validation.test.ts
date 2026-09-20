// The deploy trigger must list every path the deployed server is BUILT from.
//
// A path missing from `.github/workflows/deploy.yml`'s `on.push.paths` fails
// nothing and says nothing: the PR merges green, no deploy runs, and production
// keeps serving the old code until some unrelated push happens to touch a path
// that IS listed. It has happened once already - after A10 the trigger still
// watched the repo-root `supabase/**`, which had stopped existing, and the
// server went stale for weeks with every check green.
//
// The list cannot be maintained by memory, because the dependency is invisible
// from the workflow: the edge functions import through the `@sdk/` and `@api/`
// aliases in each function's `deno.json`, so `sdk/ts/wire/view.ts` is compiled
// into the deployed server while looking, from the workflow's side, like
// unrelated client code.
//
// So this walks the functions' REAL import graph - every entrypoint, every
// static import and re-export, transitively, aliases resolved the way Deno
// resolves them - plus the static assets named in `config.toml`, and asserts
// each one is covered. It also asserts a NEW file dropped next to an existing
// dependency would be covered, so a trigger narrowed to today's exact files
// cannot pass.
//
// Pure test - no Postgres, no network.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { join, resolve, dirname, relative } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');
const SUPA = join(REPO, 'server/impls/supabase');
const FUNCTIONS = join(SUPA, 'functions');
const WORKFLOW = join(REPO, '.github/workflows/deploy.yml');

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

/** The transitive closure of what `supabase functions deploy` compiles. */
function compiledFiles(): string[] {
    const entrypoints = readdirSync(FUNCTIONS)
        .map((name) => join(FUNCTIONS, name, 'index.ts'))
        .filter((p) => existsSync(p) && statSync(p).isFile());
    assert.ok(entrypoints.length > 0, 'found no edge-function entrypoints - did the layout move?');

    const seen = new Set<string>();
    const queue = [...entrypoints];
    while (queue.length) {
        const f = queue.pop()!;
        if (seen.has(f) || !existsSync(f)) continue;
        seen.add(f);
        for (const m of readFileSync(f, 'utf8').matchAll(SPEC)) {
            const target = resolveSpec(m[1], f);
            if (target && existsSync(target) && !seen.has(target)) queue.push(target);
        }
    }
    return [...seen].map((p) => relative(REPO, p));
}

/** The assets bundled alongside the functions, from config.toml's static_files. */
function staticFiles(): string[] {
    const toml = readFileSync(join(SUPA, 'config.toml'), 'utf8');
    const out: string[] = [];
    for (const line of toml.split('\n')) {
        const trimmed = line.trim();
        if (trimmed.startsWith('#') || !trimmed.startsWith('static_files')) continue;
        for (const m of trimmed.matchAll(/"([^"]+)"/g)) {
            out.push(relative(REPO, resolve(SUPA, m[1])));
        }
    }
    return [...new Set(out)];   // every function bundles the same wasm blob
}

/** `on.push.paths` from the workflow, read as the literal list of globs. */
function triggerPaths(): string[] {
    const lines = readFileSync(WORKFLOW, 'utf8').split('\n');
    const start = lines.findIndex((l) => /^\s{4}paths:\s*$/.test(l));
    assert.ok(start >= 0, 'deploy.yml has no on.push.paths list');
    const out: string[] = [];
    for (const line of lines.slice(start + 1)) {
        const m = line.match(/^\s+-\s+'([^']+)'\s*$/);
        if (m) { out.push(m[1]); continue; }
        if (/^\s*#/.test(line) || line.trim() === '') continue;
        break;                                  // the list ended
    }
    assert.ok(out.length > 0, 'deploy.yml lists no trigger paths at all');
    return out;
}

/** GitHub's path filter, for the two wildcards this list uses. */
function matches(glob: string, path: string): boolean {
    const rx = glob
        .split('**').map((part) => part.split('*')
            .map((lit) => lit.replace(/[.+?^${}()|[\]\\]/g, '\\$&'))
            .join('[^/]*'))
        .join('.*');
    return new RegExp(`^${rx}$`).test(path);
}

const globs = triggerPaths();
const covered = (p: string) => globs.some((g) => matches(g, p));

test('every file compiled into the deployed edge functions triggers a deploy', () => {
    const uncovered = compiledFiles().filter((p) => !covered(p));
    assert.deepEqual(uncovered, [],
        'these files are deployed but a change to them would NOT trigger deploy.yml:\n'
        + `${uncovered.join('\n')}\n`
        + `trigger paths: ${globs.join(', ')}`);
});

test('every static asset bundled with the functions triggers a deploy', () => {
    const assets = staticFiles();
    assert.ok(assets.length > 0, 'config.toml declares no static_files - did bots.wasm.gz stop shipping?');
    const uncovered = assets.filter((p) => !covered(p));
    assert.deepEqual(uncovered, [], `bundled but not watched: ${uncovered.join(', ')}`);
});

test('every trigger path still names something that exists', () => {
    // THE HISTORICAL BUG, GENERALISED. `supabase/**` did not fail when the tree
    // moved to server/impls/supabase - it went on matching nothing, quietly, and
    // the server went stale for weeks with every check green. A glob that matches
    // no file in the repo is indistinguishable, from the workflow's side, from one
    // that simply had no changes this push.
    //
    // The two tests above cannot catch it: they walk the import graph and check
    // that what the graph reaches is covered. A glob covering something the graph
    // never reaches - a generator, a header, a static asset - is invisible to
    // them. That is most of this list, and all of the entries a move breaks.
    //
    // Now that the repo holds two products with a shared/ between them, a path
    // can move without any import changing, so this is checked rather than
    // remembered.
    const dead = globs.filter((g) => {
        const literal = g.replace(/\/?\*\*$/, '').replace(/\/[^/]*\*.*$/, '');
        return literal !== '' && !existsSync(join(REPO, literal));
    });
    assert.deepEqual(dead, [],
        'these deploy.yml trigger paths match nothing in the repo - they were\n'
        + 'almost certainly left behind by a move, and a path that matches nothing\n'
        + 'silently deploys nothing:\n  ' + dead.join('\n  '));
});

test('the generated-module sources are watched, wherever they now live', () => {
    // The functions import sdk/ts/gen/*, which this workflow GENERATES from the C
    // headers before bundling. The import graph stops at the generated module and
    // can never reach the generator or the headers behind it, so these entries are
    // the one part of the list nothing derives - see deploy.yml's own comment.
    //
    // They are asserted by ROLE rather than by spelling: each of these is a real
    // input to `tools/structgen/gen.sh`, and each must be covered by some glob no
    // matter which directory it is sitting in this month.
    const sources = [
        'tools/structgen/gen.sh',                  // the driver the deploy runs
        'tools/structgen/specs/view_layout.args',  // this product's spec
        'shared/tools/structgen/structgen.c',      // the generator
        'shared/tools/structgen/sg_ts.c',          // the emitter that writes the TS
        'shared/tools/llvm.mk',                    // how it finds libclang
        'shared/c/sha256.h',                       // in the layout, via msg_wire.h
        'shared/c/deal_rng.h',                     // in the layout, via game.h
        'c/src/game.h',                            // the layout itself
    ];
    const missing = sources.filter((p) => !existsSync(join(REPO, p)));
    assert.deepEqual(missing, [], 'this test names files that no longer exist - '
        + 'it is the list that is stale, not the trigger:\n  ' + missing.join('\n  '));
    const unwatched = sources.filter((p) => !covered(p));
    assert.deepEqual(unwatched, [],
        'these feed the modules the deployed functions import, but a change to\n'
        + 'them would NOT trigger a deploy:\n  ' + unwatched.join('\n  '));
});

test('a NEW file beside a deployed one would trigger a deploy too', () => {
    // A trigger listing today's exact filenames would satisfy the test above and
    // still miss tomorrow's module. Every directory the graph reaches has to be
    // covered as a directory.
    const dirs = new Set(compiledFiles().map((p) => dirname(p)));
    const blind = [...dirs].filter((d) => !covered(`${d}/a_new_module.ts`)).sort();
    assert.deepEqual(blind, [],
        `a new file in these directories would deploy nothing: ${blind.join(', ')}`);
});

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
        for (const m of t.matchAll(/"([^"]+)"/g)) out.push(relative(REPO, resolve(SUPA, m[1])));
    }
    return out;
}

test('every function that loads bots.wasm bundles it (config.toml static_files)', () => {
    // The C Table runs in every function that touches a game (Phase 4b of
    // docs/C_GAME_SHAPE_MIGRATION.md): create and delete-account load the module
    // too now, and a deploy without the asset fails on the first request.
    const loader = join(REPO, 'sdk/ts/wasm/wasm_asset.ts');
    const missing: string[] = [];
    for (const name of readdirSync(FUNCTIONS)) {
        const entry = join(FUNCTIONS, name, 'index.ts');
        if (!existsSync(entry) || !graphOf(entry).has(loader)) continue;
        if (!staticFilesOf(name).includes('sdk/ts/wasm/bots.wasm.gz')) missing.push(name);
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
            if (!existsSync(target)) problems.push(`${name}: ${alias} resolves to a missing ${relative(REPO, target)}`);
        }
    }
    assert.ok(functionNames.length > 0 && Object.keys(aliases).length > 0, 'no functions or no aliases found');
    assert.deepEqual(problems, []);
});
